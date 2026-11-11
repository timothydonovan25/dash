// The .rgf map file: a road graph with contraction-hierarchy shortcuts, as
// tools/routegraph writes it and core::route reads it (docs/navigation.md §4).
//
// Little-endian, every record fixed-size, every section at an offset from
// the header. The dash reads it through a block cache (graph.hpp), so the
// layout is about locality: nodes are numbered along a Hilbert curve, and
// each node's edges sit together.
//
// A file holds up to fourteen *variants* of the same roads: the rider's
// speed limit (none, or 50 to 75 mph in steps of 5) times dirt allowed or
// not. A contraction hierarchy cannot drop roads at query time, so "no road
// over 55" is a different graph, chosen by `Variant::flags`. Coordinates, geometry,
// names and the snapping grid are shared.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::route {

enum class RoadClass : uint8_t {
    None = 0, Trunk, Primary, Secondary, Tertiary, Unclassified,
    Residential, LivingStreet, Service, Track, Motorway,
};

enum PoiKind : uint8_t { PoiFuel = 1, PoiFood = 2, PoiParking = 3 };

// Attr::flags.
inline constexpr uint8_t kAttrOneway = 1;   // only from -> to
inline constexpr uint8_t kAttrLink   = 2;   // a ramp
inline constexpr uint8_t kAttrDirt   = 4;
inline constexpr uint8_t kAttrRoundabout = 16;

// Variant::flags: which roads the variant leaves out. Bits 8..15 are a
// speed limit in mph: nothing whose Attr::cap_mph is over it (0: none).
inline constexpr uint32_t kNoDirt     = 2;
inline constexpr uint32_t kLimitShift = 8;
inline constexpr uint32_t kLimitFlags = 0xFFu << kLimitShift;
inline constexpr uint32_t limit_flags(uint8_t max_mph) { return uint32_t(max_mph) << kLimitShift; }
inline constexpr uint8_t  limit_mph(uint32_t variant_flags) {
    return uint8_t(variant_flags >> kLimitShift);
}

inline constexpr char     kMagic[4]    = {'R', 'G', 'F', '1'};
inline constexpr uint16_t kVersion     = 2;   // 2: speed-limit variants, interstates
inline constexpr size_t   kMaxVariants = 14;
inline constexpr uint32_t kNone        = 0xFFFFFFFFu;

struct Point {
    int32_t lat_e7;
    int32_t lon_e7;
};

// One original road between two junctions. `geom` holds the points
// between them, in from -> to order.
struct Attr {
    uint32_t from;
    uint32_t to;
    uint32_t geom_off;      // index into the geometry section
    uint32_t name_off;      // byte offset into the names section; 0 is ""
    uint32_t length_dm;
    uint32_t weight_ds;     // travel time, tenths of a second
    uint16_t geom_n;
    RoadClass cls;
    uint8_t  mph;           // the tagged limit, 0 when OSM has none
    uint8_t  flags;
    // The limit the speed-limit variants judge it by: the tag, else 70 for
    // an interstate, 65 a divided highway, 55 a primary road; 0 for a
    // lesser road with no tag (taken as under any limit).
    uint8_t  cap_mph;
    uint8_t  pad[2];
};
static_assert(sizeof(Attr) == 32, "on-card layout");

// One upward edge, stored at its lower-ranked end.
struct Edge {
    uint32_t target;
    uint32_t weight_dir;    // bits 0..29 weight (ds), bit 30 low->high, bit 31 high->low
    uint32_t aux;           // bit 31: shortcut; low bits: middle node, or Attr index
};
static_assert(sizeof(Edge) == 12, "on-card layout");

inline constexpr uint32_t kEdgeUp       = 1u << 30;
inline constexpr uint32_t kEdgeDown     = 1u << 31;
inline constexpr uint32_t kWeightMask   = kEdgeUp - 1;
inline constexpr uint32_t kAuxShortcut  = 1u << 31;

struct Poi {
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint8_t  kind;
    uint8_t  pad[3];
    uint32_t name_off;
};
static_assert(sizeof(Poi) == 16, "on-card layout");

struct Variant {
    uint32_t flags;
    uint32_t first_off;     // node_count + 1 uint32: each node's first edge
    uint32_t edges_off;
    uint32_t edge_count;
};

struct Header {
    char     magic[4];
    uint16_t version;
    uint16_t variant_count;
    uint32_t osm_date;      // yyyymmdd of the OSM data, shown on the dash
    uint32_t node_count;
    uint32_t nodes_off;     // Point per node
    uint32_t attrs_off;
    uint32_t attr_count;
    uint32_t geom_off;      // Point per shape point
    uint32_t geom_count;
    uint32_t names_off;
    uint32_t names_bytes;
    uint32_t pois_off;
    uint32_t poi_count;
    // Snapping grid: grid_w x grid_h cells from (lat0, lon0), each `cell_e7`
    // square. `grid_off` holds w*h+1 uint32 offsets into `cells_off`, a
    // list of Attr indices whose bounding box touches the cell.
    int32_t  grid_lat0_e7;
    int32_t  grid_lon0_e7;
    uint32_t cell_e7;
    uint16_t grid_w;
    uint16_t grid_h;
    uint32_t grid_off;
    uint32_t cells_off;
    uint32_t cell_items;
    Variant  variants[kMaxVariants];
};
static_assert(sizeof(Header) == 304, "on-card layout");

// Whether a variant carries this road. The builder and the snapper both
// ask, so a route never starts on a road its graph does not have.
inline bool allowed(uint8_t attr_flags, uint8_t cap_mph, uint32_t variant_flags) {
    const uint8_t limit = limit_mph(variant_flags);
    if (limit && cap_mph > limit) return false;
    if ((variant_flags & kNoDirt) && (attr_flags & kAttrDirt)) return false;
    return true;
}
inline bool allowed(const Attr& a, uint32_t variant_flags) {
    return allowed(a.flags, a.cap_mph, variant_flags);
}

}  // namespace core::route
