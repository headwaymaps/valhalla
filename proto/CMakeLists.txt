## Protobuf
set(sources
  api.pb.cc
  common.pb.cc
  directions.pb.cc
  info.pb.cc
  options.pb.cc
  sign.pb.cc
  trip.pb.cc
  transit.pb.cc
  transit_fetch.pb.cc
  incidents.pb.cc
  status.pb.cc
  ${VALHALLA_SOURCE_DIR}/third_party/OSM-binary/src/fileformat.pb.cc
  ${VALHALLA_SOURCE_DIR}/third_party/OSM-binary/src/osmformat.pb.cc)
set(headers
  api.pb.h
  common.pb.h
  directions.pb.h
  info.pb.h
  options.pb.h
  sign.pb.h
  trip.pb.h
  transit.pb.h
  transit_fetch.pb.h
  incidents.pb.h
  status.pb.h
  ${VALHALLA_SOURCE_DIR}/third_party/OSM-binary/src/fileformat.pb.h
  ${VALHALLA_SOURCE_DIR}/third_party/OSM-binary/src/osmformat.pb.h)

valhalla_module(NAME proto
  SOURCES ${sources}
  HEADERS ${headers}
  INCLUDE_DIRECTORIES
    PUBLIC
      ${CMAKE_CURRENT_BINARY_DIR})
