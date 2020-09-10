#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>

#include "baldr/datetime.h"
#include "baldr/edgeinfo.h"
#include "baldr/graphconstants.h"
#include "baldr/signinfo.h"
#include "baldr/tilehierarchy.h"
#include "baldr/time_info.h"
#include "meili/match_result.h"
#include "midgard/encoded.h"
#include "midgard/logging.h"
#include "midgard/pointll.h"
#include "midgard/util.h"
#include "proto/tripcommon.pb.h"
#include "sif/costconstants.h"
#include "sif/recost.h"
#include "thor/attributes_controller.h"
#include "thor/triplegbuilder.h"

using namespace valhalla;
using namespace valhalla::baldr;
using namespace valhalla::midgard;
using namespace valhalla::sif;
using namespace valhalla::thor;

namespace {

std::vector<std::string> split(const std::string& source, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(source);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

static bool
IsConditionalActive(const uint64_t restriction, const uint64_t local_time, const uint32_t tz_index) {

  baldr::TimeDomain td(restriction);
  return baldr::DateTime::is_conditional_active(td.type(), td.begin_hrs(), td.begin_mins(),
                                                td.end_hrs(), td.end_mins(), td.dow(),
                                                td.begin_week(), td.begin_month(), td.begin_day_dow(),
                                                td.end_week(), td.end_month(), td.end_day_dow(),
                                                local_time,
                                                baldr::DateTime::get_tz_db().from_index(tz_index));
}

uint32_t
GetAdminIndex(const AdminInfo& admin_info,
              std::unordered_map<AdminInfo, uint32_t, AdminInfo::AdminInfoHasher>& admin_info_map,
              std::vector<AdminInfo>& admin_info_list) {

  uint32_t admin_index = 0;
  auto existing_admin = admin_info_map.find(admin_info);

  // If admin was not processed yet
  if (existing_admin == admin_info_map.end()) {

    // Assign new admin index
    admin_index = admin_info_list.size();

    // Add admin info to list
    admin_info_list.emplace_back(admin_info);

    // Add admin info/index pair to map
    admin_info_map.emplace(admin_info, admin_index);
  }
  // Use known admin
  else {
    admin_index = existing_admin->second;
  }
  return admin_index;
}

void AssignAdmins(const AttributesController& controller,
                  TripLeg& trip_path,
                  const std::vector<AdminInfo>& admin_info_list) {
  if (controller.category_attribute_enabled(kAdminCategory)) {
    // Assign the admins
    for (const auto& admin_info : admin_info_list) {
      TripLeg_Admin* trip_admin = trip_path.add_admin();

      // Set country code if requested
      if (controller.attributes.at(kAdminCountryCode)) {
        trip_admin->set_country_code(admin_info.country_iso());
      }

      // Set country text if requested
      if (controller.attributes.at(kAdminCountryText)) {
        trip_admin->set_country_text(admin_info.country_text());
      }

      // Set state code if requested
      if (controller.attributes.at(kAdminStateCode)) {
        trip_admin->set_state_code(admin_info.state_iso());
      }

      // Set state text if requested
      if (controller.attributes.at(kAdminStateText)) {
        trip_admin->set_state_text(admin_info.state_text());
      }
    }
  }
}

void SetShapeAttributes(const AttributesController& controller,
                        const GraphTile* tile,
                        const DirectedEdge* edge,
                        std::vector<PointLL>& shape,
                        size_t shape_begin,
                        TripLeg& trip_path,
                        double src_pct,
                        double tgt_pct,
                        double edge_seconds,
                        bool cut_for_traffic) {
  // TODO: if this is a transit edge then the costing will throw

  if (trip_path.has_shape_attributes()) {
    // A list of percent along the edge and corresponding speed (meters per second)
    std::vector<std::tuple<double, double>> speeds;
    double speed = (edge->length() * (tgt_pct - src_pct)) / edge_seconds;
    if (cut_for_traffic) {
      // TODO: we'd like to use the speed from traffic here but because there are synchronization
      // problems with those records changing between when we used them to make the path and when we
      // try to grab them again here, we instead rely on the total time from PathInfo and just do the
      // cutting for now
      const auto& traffic_speed = tile->trafficspeed(edge);
      if (traffic_speed.breakpoint1 > 0) {
        speeds.emplace_back(traffic_speed.breakpoint1 / 255.0, speed);
        if (traffic_speed.breakpoint2 > 0) {
          speeds.emplace_back(traffic_speed.breakpoint2 / 255.0, speed);
          if (traffic_speed.speed3 != UNKNOWN_TRAFFIC_SPEED_RAW) {
            speeds.emplace_back(1, speed);
          }
        }
      }
    }
    // Cap the end so that we always have something to use
    if (speeds.empty() || std::get<0>(speeds.back()) < tgt_pct) {
      speeds.emplace_back(tgt_pct, speed);
    }

    // Set the shape attributes
    double distance_total_pct = src_pct;
    auto speed_itr = std::find_if(speeds.cbegin(), speeds.cend(),
                                  [distance_total_pct](const decltype(speeds)::value_type& s) {
                                    return distance_total_pct <= std::get<0>(s);
                                  });
    for (auto i = shape_begin + 1; i < shape.size(); ++i) {
      // If there is a change in speed here we need to make a new shape point and continue from
      // there
      double distance = shape[i].Distance(shape[i - 1]); // meters
      double distance_pct = distance / edge->length();
      double next_total = distance_total_pct + distance_pct;
      size_t shift = 0;
      if (next_total > std::get<0>(*speed_itr) && std::next(speed_itr) != speeds.cend()) {
        // Calculate where the cut point should be between these two existing shape points
        auto coef =
            (std::get<0>(*speed_itr) - distance_total_pct) / (next_total - distance_total_pct);
        auto point = shape[i - 1].PointAlongSegment(shape[i], coef);
        shape.insert(shape.begin() + i, point);
        next_total = std::get<0>(*speed_itr);
        distance *= coef;
        shift = 1;
      }
      distance_total_pct = next_total;
      double time = distance / std::get<1>(*speed_itr); // seconds

      // Set shape attributes time per shape point if requested
      if (controller.attributes.at(kShapeAttributesTime)) {
        // convert time to milliseconds and then round to an integer
        trip_path.mutable_shape_attributes()->add_time((time * kMillisecondPerSec) + 0.5);
      }

      // Set shape attributes length per shape point if requested
      if (controller.attributes.at(kShapeAttributesLength)) {
        // convert length to decimeters and then round to an integer
        trip_path.mutable_shape_attributes()->add_length((distance * kDecimeterPerMeter) + 0.5);
      }

      // Set shape attributes speed per shape point if requested
      if (controller.attributes.at(kShapeAttributesSpeed)) {
        // convert speed to decimeters per sec and then round to an integer
        trip_path.mutable_shape_attributes()->add_speed((distance * kDecimeterPerMeter / time) + 0.5);
      }

      // If we just cut the shape we need to go on to the next marker only after setting the attribs
      std::advance(speed_itr, shift);
    }
  }
}

// Set the bounding box (min,max lat,lon) for the shape
void SetBoundingBox(TripLeg& trip_path, std::vector<PointLL>& shape) {
  AABB2<PointLL> bbox(shape);
  LatLng* min_ll = trip_path.mutable_bbox()->mutable_min_ll();
  min_ll->set_lat(bbox.miny());
  min_ll->set_lng(bbox.minx());
  LatLng* max_ll = trip_path.mutable_bbox()->mutable_max_ll();
  max_ll->set_lat(bbox.maxy());
  max_ll->set_lng(bbox.maxx());
}

/**
 * Removes all edges but the one with the id that we are passing
 * @param location  The location
 * @param edge_id   The edge id to keep
 */
void RemovePathEdges(valhalla::Location* location, const GraphId& edge_id) {
  auto pos = std::find_if(location->path_edges().begin(), location->path_edges().end(),
                          [&edge_id](const valhalla::Location::PathEdge& e) {
                            return e.graph_id() == edge_id;
                          });
  if (pos == location->path_edges().end()) {
    location->mutable_path_edges()->Clear();
  } else if (location->path_edges_size() > 1) {
    location->mutable_path_edges()->SwapElements(0, pos - location->path_edges().begin());
    location->mutable_path_edges()->DeleteSubrange(1, location->path_edges_size() - 1);
  }
}

/**
 *
 */
void CopyLocations(TripLeg& trip_path,
                   const valhalla::Location& origin,
                   const std::list<valhalla::Location>& throughs,
                   const valhalla::Location& dest,
                   const std::vector<PathInfo>::const_iterator path_begin,
                   const std::vector<PathInfo>::const_iterator path_end) {
  // origin
  trip_path.add_location()->CopyFrom(origin);
  auto pe = path_begin;
  RemovePathEdges(trip_path.mutable_location(trip_path.location_size() - 1), pe->edgeid);

  // throughs
  for (const auto& through : throughs) {
    // copy
    valhalla::Location* tp_through = trip_path.add_location();
    tp_through->CopyFrom(through);
    // id set
    std::unordered_set<uint64_t> ids;
    for (const auto& e : tp_through->path_edges()) {
      ids.insert(e.graph_id());
    }
    // find id
    auto found = std::find_if(pe, path_end, [&ids](const PathInfo& pi) {
      return ids.find(pi.edgeid) != ids.end();
    });
    pe = found;
    RemovePathEdges(trip_path.mutable_location(trip_path.location_size() - 1), pe->edgeid);
  }

  // destination
  trip_path.add_location()->CopyFrom(dest);
  RemovePathEdges(trip_path.mutable_location(trip_path.location_size() - 1), (path_end - 1)->edgeid);
}

/**
 * Set begin and end heading if requested.
 * @param  trip_edge  Trip path edge to add headings.
 * @param  controller Controller specifying attributes to add to trip edge.
 * @param  edge       Directed edge.
 * @param  shape      Trip shape.
 */
void SetHeadings(TripLeg_Edge* trip_edge,
                 const AttributesController& controller,
                 const DirectedEdge* edge,
                 const std::vector<PointLL>& shape,
                 const uint32_t begin_index) {
  if (controller.attributes.at(kEdgeBeginHeading) || controller.attributes.at(kEdgeEndHeading)) {
    float offset = GetOffsetForHeading(edge->classification(), edge->use());
    if (controller.attributes.at(kEdgeBeginHeading)) {
      trip_edge->set_begin_heading(
          std::round(PointLL::HeadingAlongPolyline(shape, offset, begin_index, shape.size() - 1)));
    }
    if (controller.attributes.at(kEdgeEndHeading)) {
      trip_edge->set_end_heading(
          std::round(PointLL::HeadingAtEndOfPolyline(shape, offset, begin_index, shape.size() - 1)));
    }
  }
}

void AddBssNode(TripLeg_Node* trip_node,
                const NodeInfo* node,
                const GraphId& startnode,
                const GraphTile* start_tile,
                const GraphTile* graphtile,
                const sif::mode_costing_t& mode_costing,
                const AttributesController& controller) {
  auto pedestrian_costing = mode_costing[static_cast<size_t>(TravelMode::kPedestrian)];
  auto bicycle_costing = mode_costing[static_cast<size_t>(TravelMode::kBicycle)];

  if (node->type() == NodeType::kBikeShare && pedestrian_costing && bicycle_costing) {
    auto* bss_station_info = trip_node->mutable_bss_info();
    // TODO: import more BSS data, can be used to display capacity in real time
    bss_station_info->set_name("BSS 42");
    bss_station_info->set_ref("BSS 42 ref");
    bss_station_info->set_capacity("42");
    bss_station_info->set_network("universe");
    bss_station_info->set_operator_("Douglas");
    bss_station_info->set_rent_cost(pedestrian_costing->BSSCost().secs);
    bss_station_info->set_return_cost(bicycle_costing->BSSCost().secs);
  }
}

/**
 * @param trip_node   Trip node to add transit nodes.
 * @param node        Start nodeinfo of the current edge.
 * @param startnode   Start node of the current edge.
 * @param start_tile  Tile of the start node.
 * @param graphtile   Graph tile of the current edge.
 * @param controller  Controller specifying attributes to add to trip edge.
 *
 */
void AddTransitNodes(TripLeg_Node* trip_node,
                     const NodeInfo* node,
                     const GraphId& startnode,
                     const GraphTile* start_tile,
                     const GraphTile* graphtile,
                     const AttributesController& controller) {

  if (node->type() == NodeType::kTransitStation) {
    const TransitStop* transit_station =
        start_tile->GetTransitStop(start_tile->node(startnode)->stop_index());
    TransitStationInfo* transit_station_info = trip_node->mutable_transit_station_info();

    if (transit_station) {
      // Set onstop_id if requested
      if (controller.attributes.at(kNodeTransitStationInfoOnestopId) &&
          transit_station->one_stop_offset()) {
        transit_station_info->set_onestop_id(graphtile->GetName(transit_station->one_stop_offset()));
      }

      // Set name if requested
      if (controller.attributes.at(kNodeTransitStationInfoName) && transit_station->name_offset()) {
        transit_station_info->set_name(graphtile->GetName(transit_station->name_offset()));
      }

      // Set latitude and longitude
      LatLng* stop_ll = transit_station_info->mutable_ll();
      // Set transit stop lat/lon if requested
      if (controller.attributes.at(kNodeTransitStationInfoLatLon)) {
        PointLL ll = node->latlng(start_tile->header()->base_ll());
        stop_ll->set_lat(ll.lat());
        stop_ll->set_lng(ll.lng());
      }
    }
  }

  if (node->type() == NodeType::kTransitEgress) {
    const TransitStop* transit_egress =
        start_tile->GetTransitStop(start_tile->node(startnode)->stop_index());
    TransitEgressInfo* transit_egress_info = trip_node->mutable_transit_egress_info();

    if (transit_egress) {
      // Set onstop_id if requested
      if (controller.attributes.at(kNodeTransitEgressInfoOnestopId) &&
          transit_egress->one_stop_offset()) {
        transit_egress_info->set_onestop_id(graphtile->GetName(transit_egress->one_stop_offset()));
      }

      // Set name if requested
      if (controller.attributes.at(kNodeTransitEgressInfoName) && transit_egress->name_offset()) {
        transit_egress_info->set_name(graphtile->GetName(transit_egress->name_offset()));
      }

      // Set latitude and longitude
      LatLng* stop_ll = transit_egress_info->mutable_ll();
      // Set transit stop lat/lon if requested
      if (controller.attributes.at(kNodeTransitEgressInfoLatLon)) {
        PointLL ll = node->latlng(start_tile->header()->base_ll());
        stop_ll->set_lat(ll.lat());
        stop_ll->set_lng(ll.lng());
      }
    }
  }
}

/**
 * Add trip edge. (TODO more comments)
 * @param  controller         Controller to determine which attributes to set.
 * @param  edge               Identifier of an edge within the tiled, hierarchical graph.
 * @param  trip_id            Trip Id (0 if not a transit edge).
 * @param  block_id           Transit block Id (0 if not a transit edge)
 * @param  mode               Travel mode for the edge: Biking, walking, etc.
 * @param  directededge       Directed edge information.
 * @param  drive_right        Right side driving for this edge.
 * @param  trip_node          Trip node to add the edge information to.
 * @param  graphtile          Graph tile for accessing data.
 * @param  second_of_week     The time, from the beginning of the week in seconds at which
 *                            the path entered this edge (always monday at noon on timeless route)
 * @param  start_node_idx     The start node index
 * @param  has_junction_name  True if named junction exists, false otherwise
 * @param  start_tile         The start tile of the start node
 *
 */
TripLeg_Edge* AddTripEdge(const AttributesController& controller,
                          const GraphId& edge,
                          const uint32_t trip_id,
                          const uint32_t block_id,
                          const sif::TravelMode mode,
                          const uint8_t travel_type,
                          const std::shared_ptr<sif::DynamicCost>& costing,
                          const DirectedEdge* directededge,
                          const bool drive_on_right,
                          TripLeg_Node* trip_node,
                          const GraphTile* graphtile,
                          GraphReader& graphreader,
                          const uint32_t second_of_week,
                          const uint32_t start_node_idx,
                          const bool has_junction_name,
                          const GraphTile* start_tile,
                          const int restrictions_idx,
                          const uint64_t local_time,
                          const uint32_t tz_index) {

  // Index of the directed edge within the tile
  uint32_t idx = edge.id();

  TripLeg_Edge* trip_edge = trip_node->mutable_edge();

  // Get the edgeinfo
  auto edgeinfo = graphtile->edgeinfo(directededge->edgeinfo_offset());

  // Add names to edge if requested
  if (controller.attributes.at(kEdgeNames)) {
    auto names_and_types = edgeinfo.GetNamesAndTypes();
    for (const auto& name_and_type : names_and_types) {
      auto* trip_edge_name = trip_edge->mutable_name()->Add();
      trip_edge_name->set_value(name_and_type.first);
      trip_edge_name->set_is_route_number(name_and_type.second);
    }
  }

#ifdef LOGGING_LEVEL_TRACE
  LOG_TRACE(std::string("wayid=") + std::to_string(edgeinfo.wayid()));
#endif

  // Set the signs (if the directed edge has sign information) and if requested
  if (directededge->sign()) {
    // Add the edge signs
    std::vector<SignInfo> edge_signs = graphtile->GetSigns(idx);
    if (!edge_signs.empty()) {
      TripLeg_Sign* trip_sign = trip_edge->mutable_sign();
      for (const auto& sign : edge_signs) {
        switch (sign.type()) {
          case Sign::Type::kExitNumber: {
            if (controller.attributes.at(kEdgeSignExitNumber)) {
              auto* trip_sign_exit_number = trip_sign->mutable_exit_numbers()->Add();
              trip_sign_exit_number->set_text(sign.text());
              trip_sign_exit_number->set_is_route_number(sign.is_route_num());
            }
            break;
          }
          case Sign::Type::kExitBranch: {
            if (controller.attributes.at(kEdgeSignExitBranch)) {
              auto* trip_sign_exit_onto_street = trip_sign->mutable_exit_onto_streets()->Add();
              trip_sign_exit_onto_street->set_text(sign.text());
              trip_sign_exit_onto_street->set_is_route_number(sign.is_route_num());
            }
            break;
          }
          case Sign::Type::kExitToward: {
            if (controller.attributes.at(kEdgeSignExitToward)) {
              auto* trip_sign_exit_toward_location =
                  trip_sign->mutable_exit_toward_locations()->Add();
              trip_sign_exit_toward_location->set_text(sign.text());
              trip_sign_exit_toward_location->set_is_route_number(sign.is_route_num());
            }
            break;
          }
          case Sign::Type::kExitName: {
            if (controller.attributes.at(kEdgeSignExitName)) {
              auto* trip_sign_exit_name = trip_sign->mutable_exit_names()->Add();
              trip_sign_exit_name->set_text(sign.text());
              trip_sign_exit_name->set_is_route_number(sign.is_route_num());
            }
            break;
          }
          case Sign::Type::kGuideBranch: {
            if (controller.attributes.at(kEdgeSignGuideBranch)) {
              auto* trip_sign_guide_onto_street = trip_sign->mutable_guide_onto_streets()->Add();
              trip_sign_guide_onto_street->set_text(sign.text());
              trip_sign_guide_onto_street->set_is_route_number(sign.is_route_num());
            }
            break;
          }
          case Sign::Type::kGuideToward: {
            if (controller.attributes.at(kEdgeSignGuideToward)) {
              auto* trip_sign_guide_toward_location =
                  trip_sign->mutable_guide_toward_locations()->Add();
              trip_sign_guide_toward_location->set_text(sign.text());
              trip_sign_guide_toward_location->set_is_route_number(sign.is_route_num());
            }
            break;
          }
          case Sign::Type::kGuidanceViewJunction: {
            if (controller.attributes.at(kEdgeSignGuidanceViewJunction)) {
              auto* trip_sign_guidance_view_junction =
                  trip_sign->mutable_guidance_view_junctions()->Add();
              trip_sign_guidance_view_junction->set_text(sign.text());
              trip_sign_guidance_view_junction->set_is_route_number(sign.is_route_num());
            }
            break;
          }
        }
      }
    }
  }

  // Process the named junctions at nodes
  if (has_junction_name && start_tile) {
    // Add the node signs
    std::vector<SignInfo> node_signs = start_tile->GetSigns(start_node_idx, true);
    if (!node_signs.empty()) {
      TripLeg_Sign* trip_sign = trip_edge->mutable_sign();
      for (const auto& sign : node_signs) {
        switch (sign.type()) {
          case Sign::Type::kJunctionName: {
            if (controller.attributes.at(kEdgeSignJunctionName)) {
              auto* trip_sign_junction_name = trip_sign->mutable_junction_names()->Add();
              trip_sign_junction_name->set_text(sign.text());
              trip_sign_junction_name->set_is_route_number(sign.is_route_num());
            }
            break;
          }
        }
      }
    }
  }

  // If turn lanes exist
  if (directededge->turnlanes()) {
    auto turnlanes = graphtile->turnlanes(idx);
    for (auto tl : turnlanes) {
      TurnLane* turn_lane = trip_edge->add_turn_lanes();
      turn_lane->set_directions_mask(tl);
    }
  }

  // Set road class if requested
  if (controller.attributes.at(kEdgeRoadClass)) {
    trip_edge->set_road_class(GetRoadClass(directededge->classification()));
  }

  // Set speed if requested
  if (controller.attributes.at(kEdgeSpeed)) {
    // TODO: if this is a transit edge then the costing will throw
    // TODO: could get better precision speed here by calling GraphTile::GetSpeed but we'd need to
    // know whether or not the costing actually cares about the speed of the edge. Perhaps a
    // refactor of costing to have a GetSpeed function which EdgeCost calls internally but which we
    // can also call externally
    auto speed = directededge->length() /
                 costing->EdgeCost(directededge, graphtile, second_of_week).secs * 3.6;
    trip_edge->set_speed(speed);
  }

  uint8_t kAccess = 0;
  if (mode == sif::TravelMode::kBicycle) {
    kAccess = kBicycleAccess;
  } else if (mode == sif::TravelMode::kDrive) {
    kAccess = kAutoAccess;
  } else if (mode == sif::TravelMode::kPedestrian || mode == sif::TravelMode::kPublicTransit) {
    kAccess = kPedestrianAccess;
  }

  // Test whether edge is traversed forward or reverse
  if (directededge->forward()) {
    // Set traversability for forward directededge if requested
    if (controller.attributes.at(kEdgeTraversability)) {
      if ((directededge->forwardaccess() & kAccess) && (directededge->reverseaccess() & kAccess)) {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kBoth);
      } else if ((directededge->forwardaccess() & kAccess) &&
                 !(directededge->reverseaccess() & kAccess)) {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kForward);
      } else if (!(directededge->forwardaccess() & kAccess) &&
                 (directededge->reverseaccess() & kAccess)) {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kBackward);
      } else {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kNone);
      }
    }
  } else {
    // Set traversability for reverse directededge if requested
    if (controller.attributes.at(kEdgeTraversability)) {
      if ((directededge->forwardaccess() & kAccess) && (directededge->reverseaccess() & kAccess)) {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kBoth);
      } else if (!(directededge->forwardaccess() & kAccess) &&
                 (directededge->reverseaccess() & kAccess)) {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kForward);
      } else if ((directededge->forwardaccess() & kAccess) &&
                 !(directededge->reverseaccess() & kAccess)) {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kBackward);
      } else {
        trip_edge->set_traversability(TripLeg_Traversability::TripLeg_Traversability_kNone);
      }
    }
  }

  if (directededge->laneconnectivity()) { // && controller.attributes.at(kEdgeLaneConnectivity)) {
    for (const auto& l : graphtile->GetLaneConnectivity(idx)) {
      TripLeg_LaneConnectivity* path_lane = trip_edge->add_lane_connectivity();
      path_lane->set_from_way_id(l.from());
      path_lane->set_to_lanes(l.to_lanes());
      path_lane->set_from_lanes(l.from_lanes());

      std::cout << "|lane connectivity| wayid:" << edgeinfo.wayid() << " from wayid: " << l.from()
                << " from lanes: " << l.from_lanes() << " to lanes: " << l.to_lanes() << std::endl;

      if (directededge->access_restriction()) {
        const std::vector<baldr::AccessRestriction>& restrictions =
            graphtile->GetAccessRestrictions(edge.id(), kAllAccess, true);
        for (const auto& r : restrictions) {

          baldr::TimeDomain td(r.value());
          auto tokens = split(l.to_lanes(), '|');

          for (const auto& t : tokens) {

            std::string res =
                (r.lanes() & (1ULL << static_cast<uint32_t>(std::stoul(t)))) ? "true" : "false";

            if (r.type() == AccessType::kCenterLane && res == "true") {
              std::cout << std::endl;
              std::cout << "|center turn lane| lane " << t << std::endl;
              break;

            } else if (r.type() == baldr::AccessType::kLaneTimedAllowed ||
                       r.type() == baldr::AccessType::kLaneTimedDenied) {

              std::cout << std::endl;
              std::cout << "|timed access restriction| lane: " << t
                        << " does this restriction apply to this lane: " << res << std::endl;

              if (res == "true" && local_time && tz_index) {
                std::cout << "type: " << (int)td.type() << " beging hrs: " << (int)td.begin_hrs()
                          << " begin mins: " << (int)td.begin_mins()
                          << " end hrs: " << (int)td.end_hrs() << " end mins " << (int)td.end_mins()
                          << " dow: " << (int)td.dow() << " begin week: " << (int)td.begin_week()
                          << " begin month: " << (int)td.begin_month()
                          << " begin dow: " << (int)td.begin_day_dow()
                          << " end week: " << (int)td.end_week()
                          << " end month: " << (int)td.end_month()
                          << " end dow: " << (int)td.end_day_dow() << std::endl;

                if (IsConditionalActive(r.value(), local_time, tz_index)) {

                  if (r.type() == baldr::AccessType::kLaneTimedAllowed) {
                    if (r.modes() & costing->access_mode())
                      std::cout << "allowed" << std::endl << std::endl;
                    else
                      std::cout << "restricted" << std::endl << std::endl;
                  } else {
                    if (r.modes() & costing->access_mode())
                      std::cout << "restricted" << std::endl << std::endl;
                    else
                      std::cout << "allowed" << std::endl << std::endl;
                  }
                } else
                  std::cout << "allowed" << std::endl << std::endl;
              } else if (res == "true" && !local_time) {
                if (r.type() == baldr::AccessType::kLaneTimedAllowed) {
                  if (r.modes() & costing->access_mode())
                    std::cout << "allowed: no date time specified" << std::endl << std::endl;
                  else
                    std::cout << "restricted: no date time specified" << std::endl << std::endl;
                } else {
                  if (r.modes() & costing->access_mode())
                    std::cout << "restricted: no date time specified" << std::endl << std::endl;
                  else
                    std::cout << "allowed: no date time specified" << std::endl << std::endl;
                }
              }
            } else if (r.type() == baldr::AccessType::kLaneAllowed ||
                       r.type() == baldr::AccessType::kLaneDenied) {
              std::cout << std::endl;
              std::cout << "|non-timed access restriction| lane: " << t
                        << " does this restriction apply to this lane: " << res << " for "
                        << r.modes() << std::endl;
            }
          }
        }
      }
    }
  }

  if (directededge->end_restriction()) // if there is a restriction here.
  {
    auto restrictions =
        graphtile->GetRestrictions(true, edge, kAllAccess, true); // only get lane restrictions.
    if (restrictions.size() != 0) {
      for (const auto& cr : restrictions) {

        // cr->type() == RestrictionType::kLaneRestriction || cr->type() ==
        // RestrictionType::kComplexLane)

        if (cr->type() == RestrictionType::kComplexLane) {
          std::cout << std::endl;
          std::cout << "|complex lane| " << std::endl << std::endl;

          // Walk all vias
          std::vector<GraphId> vias;
          cr->WalkVias([&vias](const GraphId* via) {
            vias.push_back(*via);
            return WalkingVia::KeepWalking;
          });

          for (const auto& v : vias) {
            std::cout << "via graphid: " << v << std::endl;
          }

        } else if (cr->type() == RestrictionType::kLaneRestriction) {

          std::cout << std::endl;
          std::cout << "|complex restricted lane| "
                    << "type: " << cr->dt_type() << " beging hrs: " << cr->begin_hrs()
                    << " begin mins: " << cr->begin_mins() << " end hrs: " << cr->end_hrs()
                    << " end mins " << cr->end_mins() << " dow: " << cr->dow()
                    << " begin week: " << cr->begin_week() << " begin month: " << cr->begin_month()
                    << " begin dow: " << cr->begin_day_dow() << " end week: " << cr->end_week()
                    << " end month: " << cr->end_month() << " end dow: " << cr->end_day_dow()
                    << std::endl
                    << std::endl;

          if (local_time && tz_index) {
            if (cr->modes() & costing->access_mode()) {
              if (baldr::DateTime::is_conditional_active(cr->dt_type(), cr->begin_hrs(),
                                                         cr->begin_mins(), cr->end_hrs(),
                                                         cr->end_mins(), cr->dow(), cr->begin_week(),
                                                         cr->begin_month(), cr->begin_day_dow(),
                                                         cr->end_week(), cr->end_month(),
                                                         cr->end_day_dow(), local_time,
                                                         baldr::DateTime::get_tz_db().from_index(
                                                             tz_index))) {
                std::cout << "restricted" << std::endl << std::endl;
              } else
                std::cout << "allowed" << std::endl << std::endl;
            } else
              std::cout << "allowed" << std::endl << std::endl;
          } else {
            if (cr->has_dt() && cr->modes() & costing->access_mode())
              std::cout << "restricted: no date time specified" << std::endl << std::endl;
            else
              std::cout << "allowed: no date time specified" << std::endl << std::endl;
          }

          // Walk all vias
          std::vector<GraphId> vias;
          cr->WalkVias([&vias](const GraphId* via) {
            vias.push_back(*via);
            return WalkingVia::KeepWalking;
          });

          for (const auto& v : vias) {
            std::cout << "via graphid: " << v << std::endl;
          }
        }
      }
    }
  }

  if (directededge->access_restriction() && restrictions_idx >= 0) {
    const std::vector<baldr::AccessRestriction>& restrictions =
        graphtile->GetAccessRestrictions(edge.id(), costing->access_mode());
    trip_edge->mutable_restriction()->set_type(
        static_cast<uint32_t>(restrictions[restrictions_idx].type()));
  }

  trip_edge->set_has_time_restrictions(restrictions_idx >= 0);

  // Set the trip path use based on directed edge use if requested
  if (controller.attributes.at(kEdgeUse)) {
    trip_edge->set_use(GetTripLegUse(directededge->use()));
  }

  // Set toll flag if requested
  if (directededge->toll() && controller.attributes.at(kEdgeToll)) {
    trip_edge->set_toll(true);
  }

  // Set unpaved flag if requested
  if (directededge->unpaved() && controller.attributes.at(kEdgeUnpaved)) {
    trip_edge->set_unpaved(true);
  }

  // Set tunnel flag if requested
  if (directededge->tunnel() && controller.attributes.at(kEdgeTunnel)) {
    trip_edge->set_tunnel(true);
  }

  // Set bridge flag if requested
  if (directededge->bridge() && controller.attributes.at(kEdgeBridge)) {
    trip_edge->set_bridge(true);
  }

  // Set roundabout flag if requested
  if (directededge->roundabout() && controller.attributes.at(kEdgeRoundabout)) {
    trip_edge->set_roundabout(true);
  }

  // Set internal intersection flag if requested
  if (directededge->internal() && controller.attributes.at(kEdgeInternalIntersection)) {
    trip_edge->set_internal_intersection(true);
  }

  // Set drive_on_right if requested
  if (controller.attributes.at(kEdgeDriveOnRight)) {
    trip_edge->set_drive_on_right(drive_on_right);
  }

  // Set surface if requested
  if (controller.attributes.at(kEdgeSurface)) {
    trip_edge->set_surface(GetTripLegSurface(directededge->surface()));
  }

  if (directededge->destonly() && controller.attributes.at(kEdgeDestinationOnly)) {
    trip_edge->set_destination_only(directededge->destonly());
  }

  // Set the mode and travel type
  if (mode == sif::TravelMode::kBicycle) {
    // Override bicycle mode with pedestrian if dismount flag or steps
    if (directededge->dismount() || directededge->use() == Use::kSteps) {
      if (controller.attributes.at(kEdgeTravelMode)) {
        trip_edge->set_travel_mode(TripLeg_TravelMode::TripLeg_TravelMode_kPedestrian);
      }
      if (controller.attributes.at(kEdgePedestrianType)) {
        trip_edge->set_pedestrian_type(TripLeg_PedestrianType::TripLeg_PedestrianType_kFoot);
      }
    } else {
      if (controller.attributes.at(kEdgeTravelMode)) {
        trip_edge->set_travel_mode(TripLeg_TravelMode::TripLeg_TravelMode_kBicycle);
      }
      if (controller.attributes.at(kEdgeBicycleType)) {
        trip_edge->set_bicycle_type(GetTripLegBicycleType(travel_type));
      }
    }
  } else if (mode == sif::TravelMode::kDrive) {
    if (controller.attributes.at(kEdgeTravelMode)) {
      trip_edge->set_travel_mode(TripLeg_TravelMode::TripLeg_TravelMode_kDrive);
    }
    if (controller.attributes.at(kEdgeVehicleType)) {
      trip_edge->set_vehicle_type(GetTripLegVehicleType(travel_type));
    }
  } else if (mode == sif::TravelMode::kPedestrian) {
    if (controller.attributes.at(kEdgeTravelMode)) {
      trip_edge->set_travel_mode(TripLeg_TravelMode::TripLeg_TravelMode_kPedestrian);
    }
    if (controller.attributes.at(kEdgePedestrianType)) {
      trip_edge->set_pedestrian_type(GetTripLegPedestrianType(travel_type));
    }
  } else if (mode == sif::TravelMode::kPublicTransit) {
    if (controller.attributes.at(kEdgeTravelMode)) {
      trip_edge->set_travel_mode(TripLeg_TravelMode::TripLeg_TravelMode_kTransit);
    }
  }

  // Set edge id (graphid value) if requested
  if (controller.attributes.at(kEdgeId)) {
    trip_edge->set_id(edge.value);
  }

  // Set way id (base data id) if requested
  if (controller.attributes.at(kEdgeWayId)) {
    trip_edge->set_way_id(edgeinfo.wayid());
  }

  // Set weighted grade if requested
  if (controller.attributes.at(kEdgeWeightedGrade)) {
    trip_edge->set_weighted_grade((directededge->weighted_grade() - 6.f) / 0.6f);
  }

  // Set maximum upward and downward grade if requested (set to kNoElevationData if unavailable)
  if (controller.attributes.at(kEdgeMaxUpwardGrade)) {
    if (graphtile->header()->has_elevation()) {
      trip_edge->set_max_upward_grade(directededge->max_up_slope());
    } else {
      trip_edge->set_max_upward_grade(kNoElevationData);
    }
  }
  if (controller.attributes.at(kEdgeMaxDownwardGrade)) {
    if (graphtile->header()->has_elevation()) {
      trip_edge->set_max_downward_grade(directededge->max_down_slope());
    } else {
      trip_edge->set_max_downward_grade(kNoElevationData);
    }
  }

  // Set mean elevation if requested (set to kNoElevationData if unavailable)
  if (controller.attributes.at(kEdgeMeanElevation)) {
    if (graphtile->header()->has_elevation()) {
      trip_edge->set_mean_elevation(edgeinfo.mean_elevation());
    } else {
      trip_edge->set_mean_elevation(kNoElevationData);
    }
  }

  if (controller.attributes.at(kEdgeLaneCount)) {
    trip_edge->set_lane_count(directededge->lanecount());
  }

  if (directededge->cyclelane() != CycleLane::kNone && controller.attributes.at(kEdgeCycleLane)) {
    trip_edge->set_cycle_lane(GetTripLegCycleLane(directededge->cyclelane()));
  }

  if (controller.attributes.at(kEdgeBicycleNetwork)) {
    trip_edge->set_bicycle_network(directededge->bike_network());
  }

  if (controller.attributes.at(kEdgeSidewalk)) {
    if (directededge->sidewalk_left() && directededge->sidewalk_right()) {
      trip_edge->set_sidewalk(TripLeg_Sidewalk::TripLeg_Sidewalk_kBothSides);
    } else if (directededge->sidewalk_left()) {
      trip_edge->set_sidewalk(TripLeg_Sidewalk::TripLeg_Sidewalk_kLeft);
    } else if (directededge->sidewalk_right()) {
      trip_edge->set_sidewalk(TripLeg_Sidewalk::TripLeg_Sidewalk_kRight);
    }
  }

  if (controller.attributes.at(kEdgeDensity)) {
    trip_edge->set_density(directededge->density());
  }

  if (controller.attributes.at(kEdgeSpeedLimit)) {
    trip_edge->set_speed_limit(edgeinfo.speed_limit());
  }

  if (controller.attributes.at(kEdgeDefaultSpeed)) {
    trip_edge->set_default_speed(directededge->speed());
  }

  if (controller.attributes.at(kEdgeTruckSpeed)) {
    trip_edge->set_truck_speed(directededge->truck_speed());
  }

  if (directededge->truck_route() && controller.attributes.at(kEdgeTruckRoute)) {
    trip_edge->set_truck_route(true);
  }

  /////////////////////////////////////////////////////////////////////////////
  // Process transit information
  if (trip_id && (directededge->use() == Use::kRail || directededge->use() == Use::kBus)) {

    TripLeg_TransitRouteInfo* transit_route_info = trip_edge->mutable_transit_route_info();

    // Set block_id if requested
    if (controller.attributes.at(kEdgeTransitRouteInfoBlockId)) {
      transit_route_info->set_block_id(block_id);
    }

    // Set trip_id if requested
    if (controller.attributes.at(kEdgeTransitRouteInfoTripId)) {
      transit_route_info->set_trip_id(trip_id);
    }

    const TransitDeparture* transit_departure =
        graphtile->GetTransitDeparture(directededge->lineid(), trip_id,
                                       second_of_week % kSecondsPerDay);

    if (transit_departure) {

      // Set headsign if requested
      if (controller.attributes.at(kEdgeTransitRouteInfoHeadsign) &&
          transit_departure->headsign_offset()) {
        transit_route_info->set_headsign(graphtile->GetName(transit_departure->headsign_offset()));
      }

      const TransitRoute* transit_route = graphtile->GetTransitRoute(transit_departure->routeid());

      if (transit_route) {
        // Set transit type if requested
        if (controller.attributes.at(kEdgeTransitType)) {
          trip_edge->set_transit_type(GetTripLegTransitType(transit_route->route_type()));
        }

        // Set onestop_id if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoOnestopId) &&
            transit_route->one_stop_offset()) {
          transit_route_info->set_onestop_id(graphtile->GetName(transit_route->one_stop_offset()));
        }

        // Set short_name if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoShortName) &&
            transit_route->short_name_offset()) {
          transit_route_info->set_short_name(graphtile->GetName(transit_route->short_name_offset()));
        }

        // Set long_name if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoLongName) &&
            transit_route->long_name_offset()) {
          transit_route_info->set_long_name(graphtile->GetName(transit_route->long_name_offset()));
        }

        // Set color if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoColor)) {
          transit_route_info->set_color(transit_route->route_color());
        }

        // Set text_color if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoTextColor)) {
          transit_route_info->set_text_color(transit_route->route_text_color());
        }

        // Set description if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoDescription) &&
            transit_route->desc_offset()) {
          transit_route_info->set_description(graphtile->GetName(transit_route->desc_offset()));
        }

        // Set operator_onestop_id if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoOperatorOnestopId) &&
            transit_route->op_by_onestop_id_offset()) {
          transit_route_info->set_operator_onestop_id(
              graphtile->GetName(transit_route->op_by_onestop_id_offset()));
        }

        // Set operator_name if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoOperatorName) &&
            transit_route->op_by_name_offset()) {
          transit_route_info->set_operator_name(
              graphtile->GetName(transit_route->op_by_name_offset()));
        }

        // Set operator_url if requested
        if (controller.attributes.at(kEdgeTransitRouteInfoOperatorUrl) &&
            transit_route->op_by_website_offset()) {
          transit_route_info->set_operator_url(
              graphtile->GetName(transit_route->op_by_website_offset()));
        }
      }
    }
  }

  return trip_edge;
}

/**
 * Add trip intersecting edge.
 * @param  controller   Controller to determine which attributes to set.
 * @param  directededge Directed edge on the path.
 * @param  prev_de  Previous directed edge on the path.
 * @param  local_edge_index  Index of the local intersecting path edge at intersection.
 * @param  nodeinfo  Node information of the intersection.
 * @param  trip_node  Trip node that will store the intersecting edge information.
 * @param  intersecting_de Intersecting directed edge. Will be nullptr except when
 *                         on the local hierarchy.
 */
void AddTripIntersectingEdge(const AttributesController& controller,
                             const DirectedEdge* directededge,
                             const DirectedEdge* prev_de,
                             uint32_t local_edge_index,
                             const NodeInfo* nodeinfo,
                             TripLeg_Node* trip_node,
                             const DirectedEdge* intersecting_de) {
  TripLeg_IntersectingEdge* itersecting_edge = trip_node->add_intersecting_edge();

  // Set the heading for the intersecting edge if requested
  if (controller.attributes.at(kNodeIntersectingEdgeBeginHeading)) {
    itersecting_edge->set_begin_heading(nodeinfo->heading(local_edge_index));
  }

  Traversability traversability = Traversability::kNone;
  // Determine walkability
  if (intersecting_de->forwardaccess() & kPedestrianAccess) {
    traversability = (intersecting_de->reverseaccess() & kPedestrianAccess)
                         ? Traversability::kBoth
                         : Traversability::kForward;
  } else {
    traversability = (intersecting_de->reverseaccess() & kPedestrianAccess)
                         ? Traversability::kBackward
                         : Traversability::kNone;
  }
  // Set the walkability flag for the intersecting edge if requested
  if (controller.attributes.at(kNodeIntersectingEdgeWalkability)) {
    itersecting_edge->set_walkability(GetTripLegTraversability(traversability));
  }

  traversability = Traversability::kNone;
  // Determine cyclability
  if (intersecting_de->forwardaccess() & kBicycleAccess) {
    traversability = (intersecting_de->reverseaccess() & kBicycleAccess) ? Traversability::kBoth
                                                                         : Traversability::kForward;
  } else {
    traversability = (intersecting_de->reverseaccess() & kBicycleAccess) ? Traversability::kBackward
                                                                         : Traversability::kNone;
  }
  // Set the cyclability flag for the intersecting edge if requested
  if (controller.attributes.at(kNodeIntersectingEdgeCyclability)) {
    itersecting_edge->set_cyclability(GetTripLegTraversability(traversability));
  }

  // Set the driveability flag for the intersecting edge if requested
  if (controller.attributes.at(kNodeIntersectingEdgeDriveability)) {
    itersecting_edge->set_driveability(
        GetTripLegTraversability(nodeinfo->local_driveability(local_edge_index)));
  }

  // Set the previous/intersecting edge name consistency if requested
  if (controller.attributes.at(kNodeIntersectingEdgeFromEdgeNameConsistency)) {
    bool name_consistency =
        (prev_de == nullptr) ? false : prev_de->name_consistency(local_edge_index);
    itersecting_edge->set_prev_name_consistency(name_consistency);
  }

  // Set the current/intersecting edge name consistency if requested
  if (controller.attributes.at(kNodeIntersectingEdgeToEdgeNameConsistency)) {
    itersecting_edge->set_curr_name_consistency(directededge->name_consistency(local_edge_index));
  }

  // Set the use for the intersecting edge if requested
  if (controller.attributes.at(kNodeIntersectingEdgeUse)) {
    itersecting_edge->set_use(GetTripLegUse(intersecting_de->use()));
  }

  // Set the road class for the intersecting edge if requested
  if (controller.attributes.at(kNodeIntersectingEdgeRoadClass)) {
    itersecting_edge->set_road_class(GetRoadClass(intersecting_de->classification()));
  }
}

/**
 * This adds cost information at every node using supplementary costings provided at request time
 * There are some limitations here:
 * For multipoint routes the date_time used will not reflect the time offset that would have been if
 * you used the supplementary costing instead it is using the time at which the primary costing
 * arrived at the start of the leg
 * The same limitation is also true for arrive by routes in which the start time of the leg will be
 * the start time computed via the time offset from the primary costings time estimation
 * @param options     the api request options
 * @param src_pct     percent along the first edge of the path the start location snapped
 * @param tgt_pct     percent along the last edge of the path the end location snapped
 * @param date_time   date_time at the start of the leg or empty string if none
 * @param reader      graph reader for tile access
 * @param leg         the already constructed trip leg to which extra cost information is added
 */
void AccumulateRecostingInfoForward(const valhalla::Options& options,
                                    float src_pct,
                                    float tgt_pct,
                                    const std::string& date_time,
                                    valhalla::baldr::GraphReader& reader,
                                    valhalla::TripLeg& leg) {
  // bail if this is empty for some reason
  if (leg.node_size() == 0) {
    return;
  }

  // setup a callback for the recosting to get each edge
  auto in_itr = leg.node().begin();
  sif::EdgeCallback edge_cb = [&in_itr]() -> baldr::GraphId {
    auto edge_id = in_itr->has_edge() ? baldr::GraphId(in_itr->edge().id()) : baldr::GraphId{};
    ++in_itr;
    return edge_id;
  };

  // setup a callback for the recosting to tell us about the new label each made
  auto out_itr = leg.mutable_node()->begin();
  sif::LabelCallback label_cb = [&out_itr](const sif::EdgeLabel& label) -> void {
    // get the turn cost at this node
    out_itr->mutable_recosts()->rbegin()->mutable_transition_cost()->set_seconds(
        label.transition_cost().secs);
    out_itr->mutable_recosts()->rbegin()->mutable_transition_cost()->set_cost(
        label.transition_cost().cost);
    // get the elapsed time at the end of this labels edge and hang it on the next node
    ++out_itr;
    out_itr->mutable_recosts()->Add()->mutable_elapsed_cost()->set_seconds(label.cost().secs);
    out_itr->mutable_recosts()->rbegin()->mutable_elapsed_cost()->set_cost(label.cost().cost);
  };

  // do each recosting
  sif::CostFactory factory;
  for (const auto& recosting : options.recostings()) {
    // get the costing
    auto costing = factory.Create(recosting);
    // reset to the beginning of the route
    in_itr = leg.node().begin();
    out_itr = leg.mutable_node()->begin();
    // no elapsed time yet at the start of the leg
    out_itr->mutable_recosts()->Add()->mutable_elapsed_cost()->set_seconds(0);
    out_itr->mutable_recosts()->rbegin()->mutable_elapsed_cost()->set_cost(0);
    // do the recosting for this costing
    try {
      sif::recost_forward(reader, *costing, edge_cb, label_cb, src_pct, tgt_pct, date_time);
      // no turn cost at the end of the leg
      out_itr->mutable_recosts()->rbegin()->mutable_transition_cost()->set_seconds(0);
      out_itr->mutable_recosts()->rbegin()->mutable_transition_cost()->set_cost(0);
    } // couldnt be recosted (difference in access for example) so we fill it with nulls to show
      // this
    catch (...) {
      int should_have = leg.node(0).recosts_size();
      for (auto& node : *leg.mutable_node()) {
        if (node.recosts_size() == should_have) {
          node.mutable_recosts()->RemoveLast();
        }
        node.mutable_recosts()->Add();
      }
    }
  }
}

} // namespace

namespace valhalla {
namespace thor {

void TripLegBuilder::Build(
    const valhalla::Options& options,
    const AttributesController& controller,
    GraphReader& graphreader,
    const sif::mode_costing_t& mode_costing,
    const std::vector<PathInfo>::const_iterator path_begin,
    const std::vector<PathInfo>::const_iterator path_end,
    valhalla::Location& origin,
    valhalla::Location& dest,
    const std::list<valhalla::Location>& through_loc,
    TripLeg& trip_path,
    const std::function<void()>* interrupt_callback,
    std::unordered_map<size_t, std::pair<EdgeTrimmingInfo, EdgeTrimmingInfo>>* edge_trimming) {
  // Test interrupt prior to building trip path
  if (interrupt_callback) {
    (*interrupt_callback)();
  }

  // Set origin, any through locations, and destination. Origin and
  // destination are assumed to be breaks.
  CopyLocations(trip_path, origin, through_loc, dest, path_begin, path_end);
  auto* tp_orig = trip_path.mutable_location(0);
  auto* tp_dest = trip_path.mutable_location(trip_path.location_size() - 1);

  // Keep track of the time
  auto date_time = origin.has_date_time() ? origin.date_time() : "";
  baldr::DateTime::tz_sys_info_cache_t tz_cache;
  auto time_info = baldr::TimeInfo::make(origin, graphreader, &tz_cache);

  // Create an array of travel types per mode
  uint8_t travel_types[4];
  for (uint32_t i = 0; i < 4; i++) {
    travel_types[i] = (mode_costing[i] != nullptr) ? mode_costing[i]->travel_type() : 0;
  }

  // Get the first nodes graph id by using the end node of the first edge to get the tile with the
  // opposing edge then use the opposing index to get the opposing edge, and its end node is the
  // begin node of the original edge
  auto* first_edge = graphreader.GetGraphTile(path_begin->edgeid)->directededge(path_begin->edgeid);
  auto* first_tile = graphreader.GetGraphTile(first_edge->endnode());
  auto* first_node = first_tile->node(first_edge->endnode());
  GraphId startnode =
      first_tile->directededge(first_node->edge_index() + first_edge->opp_index())->endnode();

  // Partial edge at the start and side of street (sos)
  float start_pct;
  valhalla::Location::SideOfStreet start_sos =
      valhalla::Location::SideOfStreet::Location_SideOfStreet_kNone;
  PointLL start_vrt;
  for (const auto& e : origin.path_edges()) {
    if (e.graph_id() == path_begin->edgeid) {
      start_pct = e.percent_along();
      start_sos = e.side_of_street();
      start_vrt = PointLL(e.ll().lng(), e.ll().lat());
      break;
    }
  }

  // Set the origin projected location
  LatLng* proj_ll = tp_orig->mutable_projected_ll();
  proj_ll->set_lat(start_vrt.lat());
  proj_ll->set_lng(start_vrt.lng());

  // Set the origin side of street, if one exists
  if (start_sos != valhalla::Location::SideOfStreet::Location_SideOfStreet_kNone) {
    tp_orig->set_side_of_street(GetTripLegSideOfStreet(start_sos));
  }

  // Partial edge at the end
  float end_pct;
  valhalla::Location::SideOfStreet end_sos =
      valhalla::Location::SideOfStreet::Location_SideOfStreet_kNone;
  PointLL end_vrt;
  for (const auto& e : dest.path_edges()) {
    if (e.graph_id() == (path_end - 1)->edgeid) {
      end_pct = e.percent_along();
      end_sos = e.side_of_street();
      end_vrt = PointLL(e.ll().lng(), e.ll().lat());
      break;
    }
  }

  // Set the destination projected location
  proj_ll = tp_dest->mutable_projected_ll();
  proj_ll->set_lat(end_vrt.lat());
  proj_ll->set_lng(end_vrt.lng());

  // Set the destination side of street, if one exists
  if (end_sos != valhalla::Location::SideOfStreet::Location_SideOfStreet_kNone) {
    tp_dest->set_side_of_street(GetTripLegSideOfStreet(end_sos));
  }

  // Structures to process admins
  std::unordered_map<AdminInfo, uint32_t, AdminInfo::AdminInfoHasher> admin_info_map;
  std::vector<AdminInfo> admin_info_list;

  // initialize shape_attributes
  if (controller.category_attribute_enabled(kShapeAttributesCategory)) {
    trip_path.mutable_shape_attributes();
  }

  // If the path was only one edge we have a special case
  if ((path_end - path_begin) == 1) {
    const GraphTile* tile = graphreader.GetGraphTile(path_begin->edgeid);
    const DirectedEdge* edge = tile->directededge(path_begin->edgeid);

    // Get the shape. Reverse if the directed edge direction does
    // not match the traversal direction (based on start and end percent).
    auto shape = tile->edgeinfo(edge->edgeinfo_offset()).shape();
    if (edge->forward() != (start_pct < end_pct)) {
      std::reverse(shape.begin(), shape.end());
    }

    // If traversing the opposing direction: adjust start and end percent
    // and reverse the edge and side of street if traversing the opposite
    // direction
    if (start_pct > end_pct) {
      start_pct = 1.0f - start_pct;
      end_pct = 1.0f - end_pct;
      edge = graphreader.GetOpposingEdge(path_begin->edgeid, tile);
      if (end_sos == valhalla::Location::SideOfStreet::Location_SideOfStreet_kLeft) {
        tp_dest->set_side_of_street(
            GetTripLegSideOfStreet(valhalla::Location::SideOfStreet::Location_SideOfStreet_kRight));
      } else if (end_sos == valhalla::Location::SideOfStreet::Location_SideOfStreet_kRight) {
        tp_dest->set_side_of_street(
            GetTripLegSideOfStreet(valhalla::Location::SideOfStreet::Location_SideOfStreet_kLeft));
      }
    }

    float total = static_cast<float>(edge->length());
    trim_shape(start_pct * total, start_vrt, end_pct * total, end_vrt, shape);

    // Driving on right from the start of the edge?
    const GraphId start_node = graphreader.GetOpposingEdge(path_begin->edgeid)->endnode();
    bool drive_on_right = graphreader.nodeinfo(start_node)->drive_on_right();

    // Add trip edge
    auto costing = mode_costing[static_cast<uint32_t>(path_begin->mode)];
    auto trip_edge =
        AddTripEdge(controller, path_begin->edgeid, path_begin->trip_id, 0, path_begin->mode,
                    travel_types[static_cast<int>(path_begin->mode)], costing, edge, drive_on_right,
                    trip_path.add_node(), tile, graphreader, time_info.second_of_week, startnode.id(),
                    false, nullptr, path_begin->restriction_index, 0, 0);

    // Set length if requested. Convert to km
    if (controller.attributes.at(kEdgeLength)) {
      float km = std::max((edge->length() * kKmPerMeter * std::abs(end_pct - start_pct)), 0.001f);
      trip_edge->set_length(km);
    }

    // Set shape attributes
    auto edge_seconds = path_begin->elapsed_cost.secs - path_begin->transition_cost.secs;
    SetShapeAttributes(controller, tile, edge, shape, 0, trip_path, start_pct, end_pct, edge_seconds,
                       costing->flow_mask() & kCurrentFlowMask);

    // Set begin shape index if requested
    if (controller.attributes.at(kEdgeBeginShapeIndex)) {
      trip_edge->set_begin_shape_index(0);
    }
    // Set end shape index if requested
    if (controller.attributes.at(kEdgeEndShapeIndex)) {
      trip_edge->set_end_shape_index(shape.size() - 1);
    }

    // Set begin and end heading if requested. Uses shape so
    // must be done after the edge's shape has been added.
    SetHeadings(trip_edge, controller, edge, shape, 0);

    auto* node = trip_path.add_node();
    if (controller.attributes.at(kNodeElapsedTime)) {
      node->mutable_cost()->mutable_elapsed_cost()->set_seconds(path_begin->elapsed_cost.secs);
      node->mutable_cost()->mutable_elapsed_cost()->set_cost(path_begin->elapsed_cost.cost);
    }

    const GraphTile* end_tile = graphreader.GetGraphTile(edge->endnode());
    if (end_tile == nullptr) {
      if (controller.attributes.at(kNodeaAdminIndex)) {
        node->set_admin_index(0);
      }
    } else {
      if (controller.attributes.at(kNodeaAdminIndex)) {
        node->set_admin_index(
            GetAdminIndex(end_tile->admininfo(end_tile->node(edge->endnode())->admin_index()),
                          admin_info_map, admin_info_list));
      }
    }

    // Set the bounding box of the shape
    SetBoundingBox(trip_path, shape);

    // Set shape if requested
    if (controller.attributes.at(kShape)) {
      trip_path.set_shape(encode<std::vector<PointLL>>(shape));
    }

    if (controller.attributes.at(kOsmChangeset)) {
      trip_path.set_osm_changeset(tile->header()->dataset_id());
    }

    // Assign the trip path admins
    AssignAdmins(controller, trip_path, admin_info_list);

    // Add that extra costing information if requested
    AccumulateRecostingInfoForward(options, start_pct, end_pct, date_time, graphreader, trip_path);

    // Trivial path is done
    return;
  }

  // Iterate through path
  bool is_first_edge = true;
  uint32_t block_id = 0;
  uint32_t prior_opp_local_index = -1;
  std::vector<PointLL> trip_shape;
  std::string arrival_time;
  bool assumed_schedule = false;
  uint64_t osmchangeset = 0;
  size_t edge_index = 0;
  const DirectedEdge* prev_de = nullptr;
  const GraphTile* graphtile = nullptr;
  // TODO: this is temp until we use transit stop type from transitland
  TransitPlatformInfo_Type prev_transit_node_type = TransitPlatformInfo_Type_kStop;

  for (auto edge_itr = path_begin; edge_itr != path_end; ++edge_itr, ++edge_index) {
    const GraphId& edge = edge_itr->edgeid;
    const uint32_t trip_id = edge_itr->trip_id;
    graphtile = graphreader.GetGraphTile(edge, graphtile);
    const DirectedEdge* directededge = graphtile->directededge(edge);
    const sif::TravelMode mode = edge_itr->mode;
    const uint8_t travel_type = travel_types[static_cast<uint32_t>(mode)];
    const auto& costing = mode_costing[static_cast<uint32_t>(mode)];

    // Set node attributes - only set if they are true since they are optional
    const GraphTile* start_tile = graphtile;
    start_tile = graphreader.GetGraphTile(startnode, start_tile);
    const NodeInfo* node = start_tile->node(startnode);

    if (osmchangeset == 0 && controller.attributes.at(kOsmChangeset)) {
      osmchangeset = start_tile->header()->dataset_id();
    }

    // have to always compute the offset in case the timezone changes along the path
    // we could cache the timezone and just add seconds when the timezone doesnt change
    time_info = time_info.forward(trip_path.node_size() == 0
                                      ? 0.0
                                      : trip_path.node().rbegin()->cost().elapsed_cost().seconds(),
                                  node->timezone());

    // Add a node to the trip path and set its attributes.
    TripLeg_Node* trip_node = trip_path.add_node();

    if (controller.attributes.at(kNodeType)) {
      trip_node->set_type(GetTripLegNodeType(node->type()));
    }

    if (node->intersection() == IntersectionType::kFork) {
      if (controller.attributes.at(kNodeFork)) {
        trip_node->set_fork(true);
      }
    }

    // Assign the elapsed time from the start of the leg
    if (controller.attributes.at(kNodeElapsedTime)) {
      if (edge_itr == path_begin) {
        trip_node->mutable_cost()->mutable_elapsed_cost()->set_seconds(0);
        trip_node->mutable_cost()->mutable_elapsed_cost()->set_cost(0);
      } else {
        trip_node->mutable_cost()->mutable_elapsed_cost()->set_seconds(
            std::prev(edge_itr)->elapsed_cost.secs);
        trip_node->mutable_cost()->mutable_elapsed_cost()->set_cost(
            std::prev(edge_itr)->elapsed_cost.cost);
      }
    }

    // std::cout << "sec " << (time_info.local_time) << " " << (edge_itr->elapsed_cost.secs -
    // edge_itr->transition_cost.secs) << " "
    //         << DateTime::get_duration(origin.date_time(),
    //                                 edge_itr->elapsed_cost.secs - edge_itr->transition_cost.secs,
    //                               DateTime::get_tz_db().from_index(node->timezone()))
    //  << std::endl;

    // Assign the admin index
    if (controller.attributes.at(kNodeaAdminIndex)) {
      trip_node->set_admin_index(
          GetAdminIndex(start_tile->admininfo(node->admin_index()), admin_info_map, admin_info_list));
    }

    if (controller.attributes.at(kNodeTimeZone)) {
      auto tz = DateTime::get_tz_db().from_index(node->timezone());
      if (tz) {
        trip_node->set_time_zone(tz->name());
      }
    }

    if (controller.attributes.at(kNodeTransitionTime)) {
      trip_node->mutable_cost()->mutable_transition_cost()->set_seconds(
          edge_itr->transition_cost.secs);
      trip_node->mutable_cost()->mutable_transition_cost()->set_cost(edge_itr->transition_cost.cost);
    }

    AddBssNode(trip_node, node, startnode, start_tile, graphtile, mode_costing, controller);
    AddTransitNodes(trip_node, node, startnode, start_tile, graphtile, controller);

    ///////////////////////////////////////////////////////////////////////////
    // Add transit information if this is a transit stop. TODO - can we move
    // this to another method?
    if (node->is_transit()) {
      // Get the transit stop information and add transit stop info
      const TransitStop* transit_platform = start_tile->GetTransitStop(node->stop_index());
      TransitPlatformInfo* transit_platform_info = trip_node->mutable_transit_platform_info();

      // TODO: for now we will set to station for rail and stop for others
      //       in future, we will set based on transitland value
      // Set type
      if (directededge->use() == Use::kRail) {
        // Set node transit info type if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoType)) {
          transit_platform_info->set_type(TransitPlatformInfo_Type_kStation);
        }
        prev_transit_node_type = TransitPlatformInfo_Type_kStation;
      } else if (directededge->use() == Use::kPlatformConnection) {
        // Set node transit info type if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoType)) {
          transit_platform_info->set_type(prev_transit_node_type);
        }
      } else { // bus logic
        // Set node transit info type if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoType)) {
          transit_platform_info->set_type(TransitPlatformInfo_Type_kStop);
        }
        prev_transit_node_type = TransitPlatformInfo_Type_kStop;
      }

      if (transit_platform) {
        // Set onstop_id if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoOnestopId) &&
            transit_platform->one_stop_offset()) {
          transit_platform_info->set_onestop_id(
              graphtile->GetName(transit_platform->one_stop_offset()));
        }

        // Set name if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoName) &&
            transit_platform->name_offset()) {
          transit_platform_info->set_name(graphtile->GetName(transit_platform->name_offset()));
        }

        // save station name and info for all platforms.
        const DirectedEdge* dir_edge = start_tile->directededge(node->edge_index());
        for (uint32_t index = 0; index < node->edge_count(); ++index, dir_edge++) {
          if (dir_edge->use() == Use::kPlatformConnection) {
            GraphId endnode = dir_edge->endnode();
            const GraphTile* endtile = graphreader.GetGraphTile(endnode);
            const NodeInfo* nodeinfo2 = endtile->node(endnode);
            const TransitStop* transit_station = endtile->GetTransitStop(nodeinfo2->stop_index());

            // Set station onstop_id if requested
            if (controller.attributes.at(kNodeTransitPlatformInfoStationOnestopId) &&
                transit_station->one_stop_offset()) {
              transit_platform_info->set_station_onestop_id(
                  endtile->GetName(transit_station->one_stop_offset()));
            }

            // Set station name if requested
            if (controller.attributes.at(kNodeTransitPlatformInfoStationName) &&
                transit_station->name_offset()) {
              transit_platform_info->set_station_name(
                  endtile->GetName(transit_station->name_offset()));
            }

            // only one de to station exists.  we are done.
            break;
          }
        }

        // Set latitude and longitude
        LatLng* stop_ll = transit_platform_info->mutable_ll();
        // Set transit stop lat/lon if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoLatLon)) {
          PointLL ll = node->latlng(start_tile->header()->base_ll());
          stop_ll->set_lat(ll.lat());
          stop_ll->set_lng(ll.lng());
        }
      }

      // Set the arrival time at this node (based on schedule from last trip
      // departure) if requested
      if (controller.attributes.at(kNodeTransitPlatformInfoArrivalDateTime) &&
          !arrival_time.empty()) {
        transit_platform_info->set_arrival_date_time(arrival_time);
      }

      // If this edge has a trip id then there is a transit departure
      if (trip_id) {

        const TransitDeparture* transit_departure =
            graphtile->GetTransitDeparture(graphtile->directededge(edge.id())->lineid(), trip_id,
                                           time_info.second_of_week % kSecondsPerDay);

        assumed_schedule = false;
        uint32_t date, day = 0;
        if (origin.has_date_time()) {
          date = DateTime::days_from_pivot_date(DateTime::get_formatted_date(origin.date_time()));

          if (graphtile->header()->date_created() > date) {
            // Set assumed schedule if requested
            if (controller.attributes.at(kNodeTransitPlatformInfoAssumedSchedule)) {
              transit_platform_info->set_assumed_schedule(true);
            }
            assumed_schedule = true;
          } else {
            day = date - graphtile->header()->date_created();
            if (day > graphtile->GetTransitSchedule(transit_departure->schedule_index())->end_day()) {
              // Set assumed schedule if requested
              if (controller.attributes.at(kNodeTransitPlatformInfoAssumedSchedule)) {
                transit_platform_info->set_assumed_schedule(true);
              }
              assumed_schedule = true;
            }
          }
        }

        // TODO: all of the duration stuff below assumes the transit departure is on the same day as
        // the origin date time. if the trip took more than one day this will not be the case and
        // negative durations can occur
        if (transit_departure) {

          std::string dt = DateTime::get_duration(origin.date_time(),
                                                  (transit_departure->departure_time() -
                                                   (time_info.second_of_week % kSecondsPerDay)),
                                                  DateTime::get_tz_db().from_index(node->timezone()));

          std::size_t found = dt.find_last_of(' '); // remove tz abbrev.
          if (found != std::string::npos) {
            dt = dt.substr(0, found);
          }

          // Set departure time from this transit stop if requested
          if (controller.attributes.at(kNodeTransitPlatformInfoDepartureDateTime)) {
            transit_platform_info->set_departure_date_time(dt);
          }

          // TODO:  set removed tz abbrev on transit_platform_info for departure.

          // Copy the arrival time for use at the next transit stop
          arrival_time = DateTime::get_duration(origin.date_time(),
                                                (transit_departure->departure_time() +
                                                 transit_departure->elapsed_time()) -
                                                    (time_info.second_of_week % kSecondsPerDay),
                                                DateTime::get_tz_db().from_index(node->timezone()));

          found = arrival_time.find_last_of(' '); // remove tz abbrev.
          if (found != std::string::npos) {
            arrival_time = arrival_time.substr(0, found);
          }

          // TODO:  set removed tz abbrev on transit_platform_info for arrival.

          // Get the block Id
          block_id = transit_departure->blockid();
        }
      } else {
        // No departing trip, set the arrival time (for next stop) to empty
        // and set block Id to 0
        arrival_time = "";
        block_id = 0;

        // Set assumed schedule if requested
        if (controller.attributes.at(kNodeTransitPlatformInfoAssumedSchedule) && assumed_schedule) {
          transit_platform_info->set_assumed_schedule(true);
        }
        assumed_schedule = false;
      }
    }

    // Add edge to the trip node and set its attributes
    TripLeg_Edge* trip_edge =
        AddTripEdge(controller, edge, trip_id, block_id, mode, travel_type, costing, directededge,
                    node->drive_on_right(), trip_node, graphtile, graphreader,
                    time_info.second_of_week, startnode.id(), node->named_intersection(), start_tile,
                    edge_itr->restriction_index, time_info.local_time, node->timezone());

    // Get the shape and set shape indexes (directed edge forward flag
    // determines whether shape is traversed forward or reverse).
    auto edgeinfo = graphtile->edgeinfo(directededge->edgeinfo_offset());
    uint32_t begin_index = (is_first_edge) ? 0 : trip_shape.size() - 1;

    // some information regarding shape/length trimming
    auto is_last_edge = edge_itr == (path_end - 1);
    float trim_start_pct = is_first_edge ? start_pct : 0;
    float trim_end_pct = is_last_edge ? end_pct : 1;

    // Process the shape for edges where a route discontinuity occurs
    if (edge_trimming && !edge_trimming->empty() && edge_trimming->count(edge_index) > 0) {
      // Get edge shape and reverse it if directed edge is not forward.
      auto edge_shape = edgeinfo.shape();
      if (!directededge->forward()) {
        std::reverse(edge_shape.begin(), edge_shape.end());
      }

      // Grab the edge begin and end info
      auto& edge_begin_info = edge_trimming->at(edge_index).first;
      auto& edge_end_info = edge_trimming->at(edge_index).second;

      // Handle partial shape for first edge
      if (is_first_edge && !edge_begin_info.trim) {
        edge_begin_info.trim = true;
        edge_begin_info.distance_along = start_pct;
        edge_begin_info.vertex = start_vrt;
      }

      // Handle partial shape for last edge
      if (is_last_edge && !edge_end_info.trim) {
        edge_end_info.trim = true;
        edge_end_info.distance_along = end_pct;
        edge_end_info.vertex = end_vrt;
      }

      // Overwrite the trimming information for the edge length now that we know what it is
      trim_start_pct = edge_begin_info.distance_along;
      trim_end_pct = edge_end_info.distance_along;

      // Trim the shape
      auto edge_length = static_cast<float>(directededge->length());
      trim_shape(edge_begin_info.distance_along * edge_length, edge_begin_info.vertex,
                 edge_end_info.distance_along * edge_length, edge_end_info.vertex, edge_shape);
      // Add edge shape to trip
      trip_shape.insert(trip_shape.end(),
                        (edge_shape.begin() + ((edge_begin_info.trim || is_first_edge) ? 0 : 1)),
                        edge_shape.end());

      // If edge_begin_info.exists and is not the first edge then increment begin_index since
      // the previous end shape index should not equal the current begin shape index because
      // of discontinuity
      if (edge_begin_info.trim && !is_first_edge) {
        ++begin_index;
      }

    } // We need to clip the shape if its at the beginning or end
    else if (is_first_edge || is_last_edge) {
      // Get edge shape and reverse it if directed edge is not forward.
      auto edge_shape = edgeinfo.shape();
      if (!directededge->forward()) {
        std::reverse(edge_shape.begin(), edge_shape.end());
      }
      float total = static_cast<float>(directededge->length());
      // Note: that this cannot be both the first and last edge, that special case is handled above
      // Trim the shape at the front for the first edge
      if (is_first_edge) {
        trim_shape(start_pct * total, start_vrt, total, edge_shape.back(), edge_shape);
      } // And at the back if its the last edge
      else {
        trim_shape(0, edge_shape.front(), end_pct * total, end_vrt, edge_shape);
      }
      // Keep the shape
      trip_shape.insert(trip_shape.end(), edge_shape.begin() + is_last_edge, edge_shape.end());
    } // Just get the shape in there in the right direction no clipping needed
    else {
      if (directededge->forward()) {
        trip_shape.insert(trip_shape.end(), edgeinfo.shape().begin() + 1, edgeinfo.shape().end());
      } else {
        trip_shape.insert(trip_shape.end(), edgeinfo.shape().rbegin() + 1, edgeinfo.shape().rend());
      }
    }

    // Set length if requested. Convert to km
    if (controller.attributes.at(kEdgeLength)) {
      float km =
          std::max(directededge->length() * kKmPerMeter * (trim_end_pct - trim_start_pct), 0.001f);
      trip_edge->set_length(km);
    }

    // Set shape attributes
    auto edge_seconds = edge_itr->elapsed_cost.secs - edge_itr->transition_cost.secs;
    if (edge_itr != path_begin)
      edge_seconds -= std::prev(edge_itr)->elapsed_cost.secs;
    SetShapeAttributes(controller, graphtile, directededge, trip_shape, begin_index, trip_path,
                       trim_start_pct, trim_end_pct, edge_seconds,
                       costing->flow_mask() & kCurrentFlowMask);

    // Set begin shape index if requested
    if (controller.attributes.at(kEdgeBeginShapeIndex)) {
      trip_edge->set_begin_shape_index(begin_index);
    }

    // Set end shape index if requested
    if (controller.attributes.at(kEdgeEndShapeIndex)) {
      trip_edge->set_end_shape_index(trip_shape.size() - 1);
    }

    // Set begin and end heading if requested. Uses trip_shape so
    // must be done after the edge's shape has been added.
    SetHeadings(trip_edge, controller, directededge, trip_shape, begin_index);

    // Add connected edges from the start node. Do this after the first trip
    // edge is added
    //
    // Our path is from 1 to 2 to 3 (nodes) to ... n nodes.
    // Each letter represents the edge info.
    // So at node 2, we will store the edge info for D and we will store the
    // intersecting edge info for B, C, E, F, and G.  We need to make sure
    // that we don't store the edge info from A and D again.
    //
    //     (X)    (3)   (X)
    //       \\   ||   //
    //      C \\ D|| E//
    //         \\ || //
    //      B   \\||//   F
    // (X)======= (2) ======(X)
    //            ||\\
    //          A || \\ G
    //            ||  \\
    //            (1)  (X)
    if (startnode.Is_Valid()) {
      // Iterate through edges on this level to find any intersecting edges
      // Follow any upwards or downward transitions
      const DirectedEdge* de = start_tile->directededge(node->edge_index());
      for (uint32_t idx1 = 0; idx1 < node->edge_count(); ++idx1, de++) {

        // Skip shortcut edges AND the opposing edge of the previous edge in the path AND
        // the current edge in the path AND the superceded edge of the current edge in the path
        // if the current edge in the path is a shortcut
        if (de->is_shortcut() || de->localedgeidx() == prior_opp_local_index ||
            de->localedgeidx() == directededge->localedgeidx() ||
            (directededge->is_shortcut() && directededge->shortcut() & de->superseded())) {
          continue;
        }

        // Add intersecting edges on the same hierarchy level and not on the path
        AddTripIntersectingEdge(controller, directededge, prev_de, de->localedgeidx(), node,
                                trip_node, de);
      }

      // Add intersecting edges on different levels (follow NodeTransitions)
      if (node->transition_count() > 0) {
        const NodeTransition* trans = start_tile->transition(node->transition_index());
        for (uint32_t i = 0; i < node->transition_count(); ++i, ++trans) {
          // Get the end node tile and its directed edges
          GraphId endnode = trans->endnode();
          const GraphTile* endtile = graphreader.GetGraphTile(endnode);
          if (endtile == nullptr) {
            continue;
          }
          const NodeInfo* nodeinfo2 = endtile->node(endnode);
          const DirectedEdge* de2 = endtile->directededge(nodeinfo2->edge_index());
          for (uint32_t idx2 = 0; idx2 < nodeinfo2->edge_count(); ++idx2, de2++) {
            // Skip shortcut edges and edges on the path
            if (de2->is_shortcut() || de2->localedgeidx() == prior_opp_local_index ||
                de2->localedgeidx() == directededge->localedgeidx()) {
              continue;
            }
            AddTripIntersectingEdge(controller, directededge, prev_de, de2->localedgeidx(), nodeinfo2,
                                    trip_node, de2);
          }
        }
      }
    }

    // Set the endnode of this directed edge as the startnode of the next edge.
    startnode = directededge->endnode();

    if (!directededge->IsTransitLine()) {
      // Save the opposing edge as the previous DirectedEdge (for name consistency)
      const GraphTile* t2 =
          directededge->leaves_tile() ? graphreader.GetGraphTile(directededge->endnode()) : graphtile;
      if (t2 == nullptr) {
        continue;
      }
      GraphId oppedge = t2->GetOpposingEdgeId(directededge);
      prev_de = t2->directededge(oppedge);
    }

    // Save the index of the opposing local directed edge at the end node
    prior_opp_local_index = directededge->opp_local_idx();

    // set is_first edge to false
    is_first_edge = false;
  }

  // Add the last node
  auto* node = trip_path.add_node();
  if (controller.attributes.at(kNodeaAdminIndex)) {
    auto* last_tile = graphreader.GetGraphTile(startnode);
    node->set_admin_index(
        GetAdminIndex(last_tile->admininfo(last_tile->node(startnode)->admin_index()), admin_info_map,
                      admin_info_list));
  }
  if (controller.attributes.at(kNodeElapsedTime)) {
    node->mutable_cost()->mutable_elapsed_cost()->set_seconds(std::prev(path_end)->elapsed_cost.secs);
    node->mutable_cost()->mutable_elapsed_cost()->set_cost(std::prev(path_end)->elapsed_cost.cost);
  }

  if (controller.attributes.at(kNodeTransitionTime)) {
    node->mutable_cost()->mutable_transition_cost()->set_seconds(0);
    node->mutable_cost()->mutable_transition_cost()->set_cost(0);
  }

  // Assign the admins
  AssignAdmins(controller, trip_path, admin_info_list);

  // Set the bounding box of the shape
  SetBoundingBox(trip_path, trip_shape);

  // Set shape if requested
  if (controller.attributes.at(kShape)) {
    trip_path.set_shape(encode<std::vector<PointLL>>(trip_shape));
  }

  if (osmchangeset != 0 && controller.attributes.at(kOsmChangeset)) {
    trip_path.set_osm_changeset(osmchangeset);
  }

  // Add that extra costing information if requested
  AccumulateRecostingInfoForward(options, start_pct, end_pct, date_time, graphreader, trip_path);
}

} // namespace thor
} // namespace valhalla
