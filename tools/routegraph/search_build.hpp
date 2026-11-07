// The search index (core/route/search.hpp) from what extract.py found:
// streets with their towns and house numbers, towns, named places.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "core/route/search.hpp"

namespace routegraph {

struct RawPlace {
    core::route::Place kind = core::route::Place::Street;
    uint8_t sub = 0;
    std::string name;
    std::string city;
    core::route::Point p{};
    std::vector<core::route::HouseNumber> nums;   // streets only
};

// extract.py's .sxi: header "SXI1" u32 entries u32 nums u32 name_bytes
// u32 osm_date, then PlaceEntry records (offsets into this file's own
// nums and names), HouseNumber records, the names.
bool read_sxi(const std::string& path, std::vector<RawPlace>& out, uint32_t& osm_date, std::string& err);

std::vector<uint8_t> build_search(const std::vector<RawPlace>& places, uint32_t osm_date);

}  // namespace routegraph
