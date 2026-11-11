// The second half of a map build: raw road graph (.rgi, from extract.py)
// -> .rgf (core/route/rgf.hpp). Host only; allocates freely.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "core/route/rgf.hpp"

namespace routegraph {

using core::route::Point;
using core::route::Poi;

// extract.py's records. Its F_* flags are core::route::kAttr*.
struct RgiEdge {
    uint32_t u, v, length_dm, name_off, geom_off;
    uint16_t geom_n;
    uint8_t  cls, mph, flags;
};

struct Rgi {
    uint32_t osm_date = 0;
    std::vector<Point>   nodes;
    std::vector<RgiEdge> edges;
    std::vector<Point>   geom;
    std::vector<char>    names;     // offset 0 is ""
    std::vector<Poi>     pois;
};

bool read_rgi(const std::string& path, Rgi& out, std::string& err);

// The speed a road is routed at, mph: the tag when there is one, else a
// default for its class; never above the bike's cruising cap.
inline constexpr int kCruiseCapMph = 65;
int route_mph(const RgiEdge& e);
// The limit the speed-limit variants judge a road by (Attr::cap_mph): its
// tag, else 70 for an interstate, 65 for a divided highway (trunk), which
// in Arkansas it mostly is, 55 for a primary road; else 0, under any limit.
int posted_mph(const RgiEdge& e);

struct Options {
    // No limit, then 50 to 75 mph by 5 (core::config::kNavLimitMph), each
    // with dirt and without.
    std::vector<uint32_t> variants = [] {
        std::vector<uint32_t> v;
        for (uint32_t dirt : {0u, core::route::kNoDirt})
            for (uint8_t mph : {0, 50, 55, 60, 65, 70, 75}) v.push_back(core::route::limit_flags(mph) | dirt);
        return v;
    }();
    uint32_t cell_e7 = 100000;         // 0.01 deg, ~1.1 km
    bool verbose = false;
};

struct Report {
    uint32_t nodes = 0, attrs = 0;
    std::vector<uint32_t> variant_edges;
    std::vector<uint32_t> variant_shortcuts;
    double contract_s = 0;
};

std::vector<uint8_t> build_rgf(const Rgi& in, const Options& opt, Report* rep = nullptr);

// Plain Dijkstra on the uncontracted roads of a variant, node to node: the
// reference the self-check and the tests hold the hierarchy against.
uint32_t reference_ds(const std::vector<uint8_t>& rgf, uint32_t variant_flags,
                      uint32_t from_node, uint32_t to_node);

}  // namespace routegraph
