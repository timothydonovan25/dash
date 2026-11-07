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
// 65 mph or more: tagged so, or an untagged divided highway (trunk), which
// in Arkansas mostly is. The alert says "likely" for the presumed ones.
bool is_fast(const RgiEdge& e);

struct Options {
    std::vector<uint32_t> variants{0, core::route::kNoFast, core::route::kNoDirt,
                                   core::route::kNoFast | core::route::kNoDirt};
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
