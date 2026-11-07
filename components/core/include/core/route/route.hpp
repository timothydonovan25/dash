// Routing on an .rgf (docs/navigation.md §5): snap the two ends to a road,
// search the contraction hierarchy from both ends, unpack the shortcuts,
// and lay the result out as a polyline with turn cues.
//
// Memory is the caller's: a SearchStorage (two hash tables and two heaps,
// PSRAM on the dash) and a Route to fill. A search that outgrows its
// pools fails with TooFar rather than allocating.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/route/graph.hpp"

namespace core::route {

// Where a point lands on the road network.
struct Snap {
    uint32_t attr = kNone;
    uint32_t along_dm = 0;     // from the attr's `from` end, along its geometry
    uint32_t length_dm = 0;    // the attr's length measured the same way
    uint32_t off_dm = 0;       // straight-line distance from the point to the road
    Point    at{};
};

// Nearest road the variant carries, within `max_dm`. False if none.
// `ring` cells around the point are searched (the grid's cells are
// header().cell_e7 wide; ~1 km in the Arkansas build).
inline constexpr uint32_t kSnapMaxDm = 5000;   // 500 m
// Within this of a 65 mph road (and nearer it than any other), the rider
// is on it: a route avoiding them has no start there.
inline constexpr uint32_t kOnFastDm = 500;     // 50 m, as Follower's off_route_dm
bool snap(Graph& g, uint32_t variant_flags, Point p, Snap& out,
          uint32_t max_dm = kSnapMaxDm, int ring = 1);

// --- the search's memory -----------------------------------------------------

struct SearchEntry {
    uint32_t node;
    uint32_t dist;
    uint32_t parent;     // kNone at a seed
    uint32_t aux;        // the Edge::aux used to get here
    uint32_t settled;
};
struct HeapItem {
    uint32_t dist;
    uint32_t node;
};

// One direction's pools. `cap` is a power of two.
struct SearchSide {
    SearchEntry* table;
    HeapItem*    heap;
    uint32_t     cap;
};

// One edge of the path, travelled a -> b.
struct Hop {
    uint32_t a;
    uint32_t b;
    uint32_t aux;
};

struct SearchMem {
    SearchSide fwd;
    SearchSide bwd;
    Hop*       path;       // the packed path
    Hop*       stack;      // unpacking
    uint32_t   hops_cap;   // of each
};

template <uint32_t N, uint32_t H = 4096>
struct SearchStorage {
    static_assert((N & (N - 1)) == 0, "a power of two");
    SearchEntry fwd_table[N];
    SearchEntry bwd_table[N];
    HeapItem    fwd_heap[N];
    HeapItem    bwd_heap[N];
    Hop         path[H];
    Hop         stack[H];
    SearchMem mem() { return {{fwd_table, fwd_heap, N}, {bwd_table, bwd_heap, N}, path, stack, H}; }
};

// --- the result --------------------------------------------------------------

// The polyline: segment i runs from pts[i] to pts[i+1] and has that
// segment's road's limit and flags.
struct RoutePoint {
    Point    p;
    uint32_t dist_dm;      // from the start
    uint32_t time_ds;      // from the start, at the graph's speeds
    uint32_t name_off;     // the road this point's segment is on
    uint8_t  mph;          // its tagged limit, 0 if none
    uint8_t  flags;        // its Attr flags
    uint8_t  pad[2];
};

enum class CueKind : uint8_t {
    Straight, SlightLeft, Left, SharpLeft, SlightRight, Right, SharpRight, UTurn, Arrive,
    // Into a roundabout; `turn_deg` is from the way in to the way out, and
    // the name is the road it leaves by.
    Roundabout,
};
const char* to_string(CueKind k);

struct Cue {
    uint32_t point;        // index into Route::pts
    uint32_t dist_dm;      // from the start
    uint32_t name_off;     // the road after the turn
    int16_t  turn_deg;     // + right, - left
    CueKind  kind;
    uint8_t  flags;        // Attr flags of the road after the turn
};

struct Route {
    RoutePoint* pts;
    uint32_t    pts_cap;
    Cue*        cues;
    uint32_t    cues_cap;

    uint32_t n_pts = 0;
    uint32_t n_cues = 0;
    uint32_t total_dm = 0;
    uint32_t total_ds = 0;
    uint32_t fast_dm = 0;          // how much of it is on 65 mph roads
    uint32_t first_fast_name = 0;  // the first of them, for the alert
    uint32_t variant_flags = 0;    // which graph it came from
    void clear() { n_pts = n_cues = total_dm = total_ds = fast_dm = first_fast_name = 0; }
};

template <uint32_t P, uint32_t C>
struct RouteStorage {
    RoutePoint pts[P];
    Cue        cues[C];
    Route make() { return Route{pts, P, cues, C}; }
};

// --- the query ---------------------------------------------------------------

enum class Status : uint8_t {
    Ok,
    NoMap,         // the graph is not open, or has no such variant
    NoStart,       // nothing the variant carries within reach of the start
    NoDest,        // ... or of the destination
    NoRoute,       // the two are not connected (an island, a oneway trap)
    TooFar,        // the search outgrew its pools
    TooLong,       // the route outgrew the Route's buffers
    ReadError,
};
const char* to_string(Status s);

struct QueryStats {
    uint32_t settled = 0;      // both directions
    uint32_t hops = 0;         // edges on the packed path
    uint32_t legs = 0;         // roads on the unpacked path
    uint32_t cache_misses = 0; // card reads
    uint32_t weight_ds = 0;    // the search's cost, start to end
};

// Route from `from` to `to` on the variant with `variant_flags`.
Status query(Graph& g, uint32_t variant_flags, Point from, Point to,
             SearchMem mem, Route& out, QueryStats* stats = nullptr);

// Same, with the ends already snapped.
Status query(Graph& g, uint32_t variant_flags, const Snap& from, const Snap& to,
             SearchMem mem, Route& out, QueryStats* stats = nullptr);

// --- geometry helpers, shared with follow --------------------------------------

// Local flat-earth metres between two points: fine over a few km, which is
// all a snap or a segment needs.
double dist_m(Point a, Point b);
// Bearing a -> b, degrees clockwise from north.
double bearing_deg(Point a, Point b);
// Signed difference b - a, -180..180.
double turn_deg(double from_deg, double to_deg);

}  // namespace core::route
