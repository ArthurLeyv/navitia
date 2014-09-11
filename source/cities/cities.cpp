/* Copyright © 2001-2014, Canal TP and/or its affiliates. All rights reserved.

This file is part of Navitia,
    the software to build cool stuff with public transport.

Hope you'll enjoy and contribute to this project,
    powered by Canal TP (www.canaltp.fr).
Help us simplify mobility and open public transport:
    a non ending quest to the responsive locomotion way of traveling!

LICENCE: This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

Stay tuned using
twitter @navitia 
IRC #navitia on freenode
https://groups.google.com/d/forum/navitia
www.navitia.io
*/

#include "cities.h"
#include <stdio.h>
#include <queue>

#include <iostream>
#include <boost/program_options.hpp>
#include <boost/geometry.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/dynamic_bitset.hpp>

#include "ed/connectors/osm_tags_reader.h"
#include "ed/connectors/osm2ed_utils.h"
#include "utils/functions.h"
#include "utils/init.h"

#include "config.h"

namespace po = boost::program_options;
namespace pt = boost::posix_time;

namespace ed { namespace connectors {

/*
 * Read relations
 * Stores admins relations and initializes nodes and ways associated to it.
 * Stores relations for associatedStreet, also initializes nodes and ways.
 */
void ReadRelationsVisitor::relation_callback(uint64_t osm_id, const CanalTP::Tags &tags, const CanalTP::References &refs) {
    auto logger = log4cplus::Logger::getInstance("log");
    const auto tmp_admin_level = tags.find("admin_level");

    std::string insee = "", postal_code = "", name = "";
    if(tmp_admin_level != tags.end() && tmp_admin_level->second == "8") {
        for (const CanalTP::Reference& ref : refs) {
            switch(ref.member_type) {
            case OSMPBF::Relation_MemberType::Relation_MemberType_WAY:
                if (ref.role == "outer" || ref.role == "" || ref.role == "exclave") {
                    cache.ways.insert(OSMWay(ref.member_id));
                }
                break;
            case OSMPBF::Relation_MemberType::Relation_MemberType_NODE:
                if (ref.role == "admin_centre" || ref.role == "admin_center") {
                    cache.nodes.insert(OSMNode(ref.member_id));
                }
                break;
            case OSMPBF::Relation_MemberType::Relation_MemberType_RELATION:
                continue;
            }
        }
        if(tags.find("ref:INSEE") != tags.end()){
            insee = tags.at("ref:INSEE");
        }
        if(tags.find("name") != tags.end()){
            name = tags.at("name");
        }
        if(tags.find("addr:postcode") != tags.end()){
            postal_code = tags.at("addr:postcode");
        }
        cache.relations.insert(OSMRelation(osm_id, refs, insee, postal_code, name,
                boost::lexical_cast<uint32_t>(tmp_admin_level->second)));
    }
}

/*
 * We stores ways they are streets.
 * We store ids of needed nodes
 */
void ReadWaysVisitor::way_callback(uint64_t osm_id, const CanalTP::Tags &, const std::vector<uint64_t> & nodes_refs) {
    auto it_way = cache.ways.find(OSMWay(osm_id));
    if (it_way == cache.ways.end()) {
        return;
    }
    for (auto osm_id : nodes_refs) {
        auto v = cache.nodes.insert(OSMNode(osm_id));
        it_way->add_node(v.first);
    }
}

/*
 * We fill needed nodes with their coordinates
 */
void ReadNodesVisitor::node_callback(uint64_t osm_id, double lon, double lat,
        const CanalTP::Tags& ) {
    auto node_it = cache.nodes.find(OSMNode(osm_id));
    if (node_it != cache.nodes.end()) {
        node_it->set_coord(lon, lat);
    }
}

/*
 *  Builds geometries of relations
 */
void OSMCache::build_relations_geometries() {
    for (const auto& relation : relations) {
        relation.build_geometry(*this);
    }
}
/*
 * Insert relations into the database
 */
void OSMCache::insert_relations() {
    PQclear(this->lotus.exec(
                "TRUNCATE  administrative_regions CASCADE;"));
    lotus.prepare_bulk_insert("administrative_regions", {"id", "name", "post_code", "insee",
            "level", "coord", "boundary", "uri"});
    size_t nb_empty_polygons = 0 ;
    for (auto relation : relations) {
        if(!relation.polygon.empty()){
            std::stringstream polygon_stream;
            polygon_stream <<  bg::wkt<mpolygon_type>(relation.polygon);
            std::string polygon_str = polygon_stream.str();
            const auto coord = "POINT(" + std::to_string(relation.centre.get<0>())
                + " " + std::to_string(relation.centre.get<1>()) + ")";
            lotus.insert({std::to_string(relation.osm_id), relation.name,
                          relation.postal_code, relation.insee,
                          std::to_string(relation.level), coord, polygon_str,
                          "admin:"+std::to_string(relation.osm_id)});
        } else {
            ++nb_empty_polygons;
        }
    }
    lotus.finish_bulk_insert();
    auto logger = log4cplus::Logger::getInstance("log");
    LOG4CPLUS_INFO(logger, "Ignored " << std::to_string(nb_empty_polygons) << " admins because their polygons were empty");
}


std::string OSMNode::to_geographic_point() const{
    std::stringstream geog;
    geog << std::setprecision(10)<<"POINT("<< coord_to_string() <<")";
    return geog.str();
}
/*
 * We build the polygon of the admin and insert it in the rtree
 */
void OSMRelation::build_geometry(OSMCache& cache) const {
    polygon_type tmp_polygon;
    for (CanalTP::Reference ref : references) {
        if (ref.member_type == OSMPBF::Relation_MemberType::Relation_MemberType_WAY) {
            auto it = cache.ways.find(OSMWay(ref.member_id));
            if (it == cache.ways.end()) {
                continue;
            }
            if(ref.role != "outer"  && ref.role != "enclave" && ref.role != "") {
                continue;
            }
            for (auto node : it->nodes) {
                if (!node->is_defined()) {
                    continue;
                }
                const auto p = point(float(node->lon()), float(node->lat()));
                tmp_polygon.outer().push_back(p);
            }
        } else if (ref.member_type == OSMPBF::Relation_MemberType::Relation_MemberType_NODE) {
            auto node_it = cache.nodes.find(OSMNode(ref.member_id));
            if (node_it == cache.nodes.end()) {
                continue;
            }
            if (!node_it->is_defined()) {
                continue;
            }
            if (ref.role == "admin_centre") {
                centre = point(float(node_it->lon()), float(node_it->lat()));
            }
        }
    }
    if (tmp_polygon.outer().size() < 2) {
        return;
    }
    if (centre.get<0>() == 0.0 || centre.get<1>() == 0.0) {
        bg::centroid(tmp_polygon, centre);
    }
    boost::geometry::model::box<point> envelope;
    bg::envelope(tmp_polygon, envelope);
    const auto front = tmp_polygon.outer().front();
    const auto back = tmp_polygon.outer().back();
    if (front.get<0>() != back.get<0>() || front.get<1>() != back.get<1>()) {
        tmp_polygon.outer().push_back(tmp_polygon.outer().front());
    }
    polygon.push_back(tmp_polygon);
}
}}

int main(int argc, char** argv) {
    navitia::init_app();
    auto logger = log4cplus::Logger::getInstance("log");
    pt::ptime start;
    std::string input, connection_string;
    po::options_description desc("Allowed options");
    desc.add_options()
        ("version,v", "Affiche la version")
        ("help,h", "Affiche l'aide")
        ("input,i", po::value<std::string>(&input)->required(), "Fichier OSM à utiliser")
        ("connection-string", po::value<std::string>(&connection_string)->required(), "parametres de connexion à la base de données: host=localhost user=navitia dbname=navitia password=navitia");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if(vm.count("version")){
        LOG4CPLUS_INFO(logger, argv[0] << " V" << KRAKEN_VERSION << " " << NAVITIA_BUILD_TYPE);
        return 0;
    }

    if(vm.count("help")) {
        LOG4CPLUS_INFO(logger, desc);
        return 1;
    }

    start = pt::microsec_clock::local_time();
    po::notify(vm);


    ed::connectors::OSMCache cache(connection_string);
    ed::connectors::ReadRelationsVisitor relations_visitor(cache);
    CanalTP::read_osm_pbf(input, relations_visitor);
    ed::connectors::ReadWaysVisitor ways_visitor(cache);
    CanalTP::read_osm_pbf(input, ways_visitor);
    ed::connectors::ReadNodesVisitor node_visitor(cache);
    CanalTP::read_osm_pbf(input, node_visitor);
    cache.build_relations_geometries();
    cache.insert_relations();
    return 0;
}
