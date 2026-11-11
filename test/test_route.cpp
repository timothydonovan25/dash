// core::route against a synthetic town built by the real map builder
// (tools/routegraph): every route the hierarchy finds must cost what
// plain Dijkstra on the uncontracted roads says.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/route/dest.hpp"
#include "core/route/route.hpp"
#include "rgf_build.hpp"

using namespace core::route;
using routegraph::Rgi;
using routegraph::RgiEdge;

namespace {

class MemSource : public IBlockSource {
public:
    explicit MemSource(std::vector<uint8_t> b) : b_(std::move(b)) {}
    bool read(uint32_t off, void* dst, uint32_t len) override {
        if (fail_after_ >= 0 && reads_++ >= fail_after_) return false;
        if (off > b_.size() || len > b_.size() - off) return false;
        std::memcpy(dst, b_.data() + off, len);
        return true;
    }
    uint32_t size() const override { return uint32_t(b_.size()); }
    std::vector<uint8_t> b_;
    int fail_after_ = -1;
    int reads_ = 0;
};

// A 6 x 6 grid of junctions 200 m apart near Fayetteville. Rows run east
// (named "Row k"), columns north ("Col k"). Row 5 is an untagged divided
// highway (judged 65); Row 4's first block is posted 70; column 2 is one
// way, north only; a dirt track runs diagonally from (0,0) to (2,2).
constexpr int kN = 6;
constexpr int32_t kLat0 = 360000000, kLon0 = -941600000;
constexpr int32_t kStepLat = 18000;          // ~200 m
constexpr int32_t kStepLon = 22300;          // ~200 m at 36 N

uint32_t id(int r, int c) { return uint32_t(r * kN + c); }
Point at(int r, int c) { return {kLat0 + r * kStepLat, kLon0 + c * kStepLon}; }

uint32_t add_name(Rgi& g, const std::string& s) {
    const uint32_t off = uint32_t(g.names.size());
    g.names.insert(g.names.end(), s.begin(), s.end());
    g.names.push_back('\0');
    return off;
}

void road(Rgi& g, uint32_t u, uint32_t v, RoadClass cls, uint8_t flags, uint32_t name, bool bend = false) {
    RgiEdge e{};
    e.u = u;
    e.v = v;
    e.name_off = name;
    e.cls = uint8_t(cls);
    e.flags = flags;
    e.geom_off = uint32_t(g.geom.size());
    const Point a = g.nodes[u], b = g.nodes[v];
    if (bend) {   // one shape point, pushed sideways a little
        g.geom.push_back({(a.lat_e7 + b.lat_e7) / 2 + 900, (a.lon_e7 + b.lon_e7) / 2 + 900});
        e.geom_n = 1;
    }
    e.length_dm = uint32_t(dist_m(a, b) * 10 * (bend ? 1.01 : 1.0));
    g.edges.push_back(e);
}

Rgi town() {
    Rgi g;
    g.osm_date = 20261102;
    g.names.push_back('\0');
    for (int r = 0; r < kN; ++r)
        for (int c = 0; c < kN; ++c) g.nodes.push_back(at(r, c));
    for (int r = 0; r < kN; ++r) {
        const uint32_t nm = add_name(g, "Row " + std::to_string(r));
        for (int c = 0; c + 1 < kN; ++c)
            road(g, id(r, c), id(r, c + 1), r == 5 ? RoadClass::Trunk : RoadClass::Residential, 0, nm, c == 3);
    }
    g.edges[size_t(4 * (kN - 1))].mph = 70;
    for (int c = 0; c < kN; ++c) {
        const uint32_t nm = add_name(g, "Col " + std::to_string(c));
        for (int r = 0; r + 1 < kN; ++r)
            road(g, id(r, c), id(r + 1, c), RoadClass::Residential, c == 2 ? kAttrOneway : 0, nm);
    }
    const uint32_t track = add_name(g, "Ridge Track");
    road(g, id(0, 0), id(1, 1), RoadClass::Track, kAttrDirt, track);
    road(g, id(1, 1), id(2, 2), RoadClass::Track, kAttrDirt, track);
    g.pois.push_back({at(3, 3).lat_e7, at(3, 3).lon_e7, PoiFuel, {0, 0, 0}, 0});
    return g;
}

struct Rig {
    BlockCacheStorage<16> cache_mem;     // 64 kB: small on purpose, so blocks get evicted
    SearchStorage<1024, 256> search;
    RouteStorage<512, 64> route_mem;
    BlockCache cache = cache_mem.make();
    Graph g{cache};
    std::unique_ptr<MemSource> src;
    Route route = route_mem.make();

    explicit Rig(std::vector<uint8_t> b) : src(new MemSource(std::move(b))) {}
};

Snap at_node(Graph& g, uint32_t n) {
    for (uint32_t i = 0; i < g.header().attr_count; ++i) {
        Attr a;
        g.attr(i, a);
        if (a.from == n) return Snap{i, 0, 1, 0, {}};
        if (a.to == n) return Snap{i, 1, 1, 0, {}};
    }
    return Snap{};
}

std::string name(Graph& g, uint32_t off) {
    char buf[64];
    g.name(off, buf, sizeof buf);
    return buf;
}

// The node the builder renumbered (r, c) to: the one at its coordinates.
uint32_t node_at(Graph& g, int r, int c) {
    for (uint32_t i = 0; i < g.header().node_count; ++i) {
        Point p;
        g.node(i, p);
        if (p.lat_e7 == at(r, c).lat_e7 && p.lon_e7 == at(r, c).lon_e7) return i;
    }
    return kNone;
}

}  // namespace

int main() {
    const Rgi rgi = town();
    routegraph::Options opt;
    const std::vector<uint8_t> file = routegraph::build_rgf(rgi, opt);
    auto rig = std::make_unique<Rig>(file);
    Graph& g = rig->g;

    CASE("the file opens and carries fourteen variants: no limit and 50 to 75 by 5, dirt or not");
    CHECK(g.open(*rig->src) == OpenResult::Ok);
    CHECK(g.header().variant_count == 14);
    CHECK(g.header().node_count == kN * kN);
    CHECK(g.header().osm_date == 20261102);
    for (uint32_t dirt : {0u, kNoDirt})
        for (uint8_t mph : {0, 50, 55, 60, 65, 70, 75}) CHECK(g.variant(limit_flags(mph) | dirt) >= 0);
    CHECK(g.variant(limit_flags(45)) < 0);

    CASE("judged limits: the untagged divided highway 65, the tag 70, untagged streets 0; the track is dirt");
    {
        int c65 = 0, c70 = 0, c0 = 0, dirt = 0;
        for (uint32_t i = 0; i < g.header().attr_count; ++i) {
            Attr a;
            g.attr(i, a);
            if (a.cap_mph == 65) { ++c65; CHECK(name(g, a.name_off) == "Row 5" && a.mph == 0); }
            else if (a.cap_mph == 70) { ++c70; CHECK(name(g, a.name_off) == "Row 4" && a.mph == 70); }
            else if (a.cap_mph == 0) ++c0;
            if (a.flags & kAttrDirt) { ++dirt; CHECK(a.cls == RoadClass::Track); }
        }
        CHECK(c65 == kN - 1);
        CHECK(c70 == 1);
        CHECK(c65 + c70 + c0 == int(g.header().attr_count));
        CHECK(dirt == 2);
        // Over the limit is out; at it is in.
        CHECK(!allowed(0, 65, limit_flags(60)) && allowed(0, 65, limit_flags(65)));
        CHECK(!allowed(0, 55, limit_flags(50)) && allowed(0, 50, limit_flags(50)));
        CHECK(allowed(0, 0, limit_flags(50)) && allowed(0, 75, 0));
        CHECK(!allowed(kAttrDirt, 0, kNoDirt) && allowed(kAttrDirt, 0, limit_flags(50)));
    }

    CASE("every node pair, every variant: the hierarchy's cost is Dijkstra's");
    {
        int mismatches = 0, routes = 0, unreachable = 0;
        for (int v = 0; v < g.header().variant_count; ++v) {
            const uint32_t vf = g.header().variants[v].flags;
            for (uint32_t s = 0; s < uint32_t(kN * kN); ++s)
                for (uint32_t t = 0; t < uint32_t(kN * kN); ++t) {
                    Snap a = at_node(g, s), b = at_node(g, t);
                    Attr aa, ab;
                    g.attr(a.attr, aa);
                    g.attr(b.attr, ab);
                    if (!allowed(aa, vf) || !allowed(ab, vf)) continue;
                    QueryStats st;
                    const Status q = query(g, vf, a, b, rig->search.mem(), rig->route, &st);
                    const uint32_t want = routegraph::reference_ds(file, vf, s, t);
                    if (want == kNone) { ++unreachable; mismatches += q != Status::NoRoute; continue; }
                    ++routes;
                    if (q != Status::Ok || st.weight_ds != want) ++mismatches;
                }
        }
        CHECK(routes > 3000);
        CHECK(mismatches == 0);
        CHECK(unreachable == 0);   // a grid: every corner reachable in every variant
    }

    CASE("a point 20 m off a road snaps to it");
    {
        Point p = at(2, 1);
        p.lon_e7 += kStepLon / 2;          // halfway along Row 2
        p.lat_e7 += 1800;                  // ~20 m north of it
        Snap s;
        CHECK(snap(g, 0, p, s));
        Attr a;
        g.attr(s.attr, a);
        CHECK(name(g, a.name_off) == "Row 2");
        CHECK_NEAR(s.off_dm, 200, 10);
        CHECK_NEAR(s.along_dm, s.length_dm / 2.0, 20);
        Point far = at(0, 0);
        far.lat_e7 -= 200000;              // 2 km south of town
        CHECK(!snap(g, 0, far, s));
    }

    CASE("an L: east along Row 1, left onto Col 4, arrive");
    {
        Point from = at(1, 0), to = at(4, 4);
        from.lon_e7 += kStepLon / 4;
        to.lat_e7 -= kStepLat / 2;         // halfway down Col 4 between rows 3 and 4
        QueryStats st;
        const Status q = query(g, kNoDirt, from, to, rig->search.mem(), rig->route, &st);
        CHECK(q == Status::Ok);
        const Route& r = rig->route;
        CHECK(r.n_pts >= 3);
        // Distance and time only grow, and the ends are where we asked.
        bool mono = true;
        for (uint32_t i = 1; i < r.n_pts; ++i)
            mono &= r.pts[i].dist_dm >= r.pts[i - 1].dist_dm && r.pts[i].time_ds >= r.pts[i - 1].time_ds;
        CHECK(mono);
        CHECK(dist_m(r.pts[0].p, from) < 1.0);
        CHECK(dist_m(r.pts[r.n_pts - 1].p, to) < 1.0);
        // 3.75 blocks east and 2.5 north, any way round on a grid.
        CHECK_NEAR(r.total_dm, 6.25 * 2000, 200);
        CHECK(r.total_dm == r.pts[r.n_pts - 1].dist_dm);
        // A single left turn somewhere (which column it takes is the
        // router's choice), then the arrival.
        CHECK(r.n_cues >= 2);
        CHECK(r.cues[r.n_cues - 1].kind == CueKind::Arrive);
        int lefts = 0;
        for (uint32_t i = 0; i + 1 < r.n_cues; ++i) {
            if (r.cues[i].kind == CueKind::Left) {
                ++lefts;
                CHECK(name(g, r.cues[i].name_off).rfind("Col ", 0) == 0);
                CHECK_NEAR(r.cues[i].turn_deg, -90, 10);   // Row 1 bends just before col 4
            }
        }
        CHECK(lefts >= 1);
    }

    CASE("the one-way column is only used northbound");
    {
        const uint32_t n42 = node_at(g, 4, 2), n12 = node_at(g, 1, 2);
        Snap s = at_node(g, n42), t = at_node(g, n12);
        QueryStats down, up;
        CHECK(query(g, 0, s, t, rig->search.mem(), rig->route, &down) == Status::Ok);
        CHECK(query(g, 0, t, s, rig->search.mem(), rig->route, &up) == Status::Ok);
        CHECK(down.weight_ds > up.weight_ds);     // southbound has to go round
    }

    CASE("along Row 5 the full graph and 65 take the highway; 55 and 60 never do");
    {
        const Point from = at(5, 0), to = at(5, 5);
        const auto on_highway = [&] {
            for (uint32_t i = 0; i < rig->route.n_pts; ++i)
                if (rig->route.pts[i].cap_mph > 55) return true;
            return false;
        };
        CHECK(query(g, 0, from, to, rig->search.mem(), rig->route) == Status::Ok);
        CHECK(on_highway());
        CHECK(query(g, limit_flags(65), from, to, rig->search.mem(), rig->route) == Status::Ok);
        CHECK(on_highway());
        // Mid-block on the highway itself, the 55 graph has no start: the
        // rider is on a road over the limit (the navigator then detours).
        // At a junction a cross street is as near, so that is a start.
        const Point mid{from.lat_e7, from.lon_e7 + kStepLon / 2};
        CHECK(query(g, limit_flags(60), mid, to, rig->search.mem(), rig->route) == Status::NoStart);
        // Beside it, it snaps to the nearest road it has and routes round.
        Point from_off = from, to_off = to;
        from_off.lat_e7 -= 3000;
        to_off.lat_e7 -= 3000;
        CHECK(query(g, limit_flags(55), from_off, to_off, rig->search.mem(), rig->route) == Status::Ok);
        CHECK(rig->route.variant_flags == limit_flags(55));
        CHECK(!on_highway());
    }

    CASE("Row 4's 70 mph block is used with no limit or 70, never under 65");
    {
        const Point from = at(4, 0), to = at(4, 1);
        CHECK(query(g, 0, from, to, rig->search.mem(), rig->route) == Status::Ok);
        const uint32_t direct = rig->route.total_dm;
        Point from_off = from, to_off = to;
        from_off.lat_e7 += 3000;
        to_off.lat_e7 += 3000;
        CHECK(query(g, limit_flags(65), from_off, to_off, rig->search.mem(), rig->route) == Status::Ok);
        CHECK(rig->route.total_dm > direct);
        for (uint32_t i = 0; i < rig->route.n_pts; ++i) CHECK(rig->route.pts[i].cap_mph <= 65);
        CHECK(query(g, limit_flags(70), from, to, rig->search.mem(), rig->route) == Status::Ok);
        CHECK(rig->route.total_dm == direct);            // 70 is at the limit, not over it
    }

    CASE("the dirt track is a shortcut only where dirt is allowed");
    {
        const Point from = at(0, 0), to = at(2, 2);
        QueryStats dirt, paved;
        CHECK(query(g, 0, from, to, rig->search.mem(), rig->route, &dirt) == Status::Ok);
        CHECK(query(g, kNoDirt, from, to, rig->search.mem(), rig->route, &paved) == Status::Ok);
        for (uint32_t i = 0; i < rig->route.n_pts; ++i) CHECK(!(rig->route.pts[i].flags & kAttrDirt));
        CHECK(paved.weight_ds >= dirt.weight_ds);
    }

    CASE("start and end on one road, both ways");
    {
        Point a = at(3, 1), b = at(3, 1);
        a.lon_e7 += kStepLon / 5;
        b.lon_e7 += kStepLon * 4 / 5;
        CHECK(query(g, 0, a, b, rig->search.mem(), rig->route) == Status::Ok);
        CHECK_NEAR(rig->route.total_dm, 1200, 30);
        CHECK(query(g, 0, b, a, rig->search.mem(), rig->route) == Status::Ok);
        CHECK_NEAR(rig->route.total_dm, 1200, 30);
        CHECK(dist_m(rig->route.pts[0].p, b) < 1.0);
    }

    CASE("pools too small: TooFar, not a crash; buffers too small: TooLong");
    {
        auto tiny = std::make_unique<SearchStorage<4, 4>>();
        CHECK(query(g, 0, at(0, 0), at(5, 5), tiny->mem(), rig->route) == Status::TooFar);
        RouteStorage<3, 4> small;
        Route r = small.make();
        CHECK(query(g, 0, at(0, 0), at(5, 5), rig->search.mem(), r) == Status::TooLong);
    }

    CASE("a roundabout is one cue, at the way in, named for the way out");
    {
        // A ring of four nodes ~40 m across, anticlockwise one way; roads
        // in from the south and out to the north, east and west.
        Rgi rb;
        rb.osm_date = 20261102;
        rb.names.push_back('\0');
        const uint32_t ring = add_name(rb, "Circle"), south = add_name(rb, "South Rd"),
                       north = add_name(rb, "North Rd"), east = add_name(rb, "East Rd"),
                       west = add_name(rb, "West Rd");
        const int32_t r = 1800, rl = 2230;       // ~20 m
        rb.nodes = {{kLat0 - r, kLon0}, {kLat0, kLon0 + rl}, {kLat0 + r, kLon0}, {kLat0, kLon0 - rl},
                    {kLat0 - 20 * r, kLon0}, {kLat0 + 20 * r, kLon0},
                    {kLat0, kLon0 + 20 * rl}, {kLat0, kLon0 - 20 * rl}};
        // S -> E -> N -> W -> S: anticlockwise, as US roundabouts run.
        for (uint32_t i = 0; i < 4; ++i)
            road(rb, i, (i + 1) % 4, RoadClass::Tertiary, kAttrOneway | kAttrRoundabout, ring);
        road(rb, 4, 0, RoadClass::Tertiary, 0, south);
        road(rb, 2, 5, RoadClass::Tertiary, 0, north);
        road(rb, 1, 6, RoadClass::Tertiary, 0, east);
        road(rb, 3, 7, RoadClass::Tertiary, 0, west);
        auto rbig = std::make_unique<Rig>(routegraph::build_rgf(rb, opt));
        CHECK(rbig->g.open(*rbig->src) == OpenResult::Ok);
        // South to west: in at the south, round past east and north, out west.
        Point from = rb.nodes[4], to = rb.nodes[7];
        from.lat_e7 += 4 * r;
        to.lon_e7 += 4 * rl;
        CHECK(query(rbig->g, 0, from, to, rbig->search.mem(), rbig->route) == Status::Ok);
        const Route& q = rbig->route;
        CHECK(q.n_cues == 2);
        CHECK(q.cues[0].kind == CueKind::Roundabout);
        CHECK(name(rbig->g, q.cues[0].name_off) == "West Rd");
        CHECK_NEAR(q.cues[0].turn_deg, -90, 10);    // heading north in, west out: a left
        CHECK(q.cues[1].kind == CueKind::Arrive);
        // Straight over: north, out on North Rd.
        to = rb.nodes[5];
        to.lat_e7 -= 4 * r;
        CHECK(query(rbig->g, 0, from, to, rbig->search.mem(), rbig->route) == Status::Ok);
        CHECK(q.n_cues == 2);
        CHECK(q.cues[0].kind == CueKind::Roundabout);
        CHECK(name(rbig->g, q.cues[0].name_off) == "North Rd");
        CHECK_NEAR(q.cues[0].turn_deg, 0, 10);
    }

    CASE("an upload's first bytes tell a map from anything else");
    {
        uint32_t date = 0;
        CHECK(map_header_ok(file.data(), file.size(), &date));
        CHECK(date == 20261102);
        CHECK(!map_header_ok(file.data(), sizeof(Header) - 1));
        std::vector<uint8_t> wad(file.begin(), file.begin() + 200);
        std::memcpy(wad.data(), "IWAD", 4);
        CHECK(!map_header_ok(wad.data(), wad.size()));
        std::vector<uint8_t> newer(file.begin(), file.begin() + 200);
        newer[4] = uint8_t(kVersion + 1);
        CHECK(!map_header_ok(newer.data(), newer.size()));
    }

    CASE("nearest fuel from the map");
    {
        Dest d[3];
        CHECK(nearest_pois(g, at(0, 0), PoiFuel, d, 3) == 1);
        CHECK(std::string(d[0].name) == "Fuel");      // unnamed in the town
        CHECK(d[0].p.lat_e7 == at(3, 3).lat_e7);
        CHECK(nearest_pois(g, at(0, 0), PoiFood, d, 3) == 0);
    }

    CASE("a damaged or foreign file is refused");
    {
        std::vector<uint8_t> bad = file;
        bad[0] = 'X';
        MemSource s1(bad);
        CHECK(g.open(s1) == OpenResult::BadMagic);
        std::vector<uint8_t> cut(file.begin(), file.begin() + file.size() / 2);
        MemSource s2(cut);
        CHECK(g.open(s2) == OpenResult::BadLayout);
        MemSource s3(file);
        s3.fail_after_ = 0;
        CHECK(g.open(s3) == OpenResult::ReadError);
        CHECK(query(g, 0, at(0, 0), at(1, 1), rig->search.mem(), rig->route) == Status::NoMap);
    }

    CASE("a read error mid-query is reported");
    {
        MemSource s(file);
        CHECK(g.open(s) == OpenResult::Ok);
        g.cache().attach(&s);              // drop the cached blocks, keep the header
        s.fail_after_ = 0;                 // the very next block read fails
        CHECK(query(g, 0, at(0, 0), at(5, 5), rig->search.mem(), rig->route) == Status::ReadError);
    }

    CASE("longest_roads: most time first, the long one's middle, small ones dropped");
    {
        // Roads by segment, 10 s each: A A B A A A C D(unnamed) E, then the end.
        static RouteStorage<16, 2> m;
        Route r = m.make();
        const uint32_t roads[] = {1, 1, 2, 1, 1, 1, 3, 0, 4};
        for (uint32_t i = 0; i <= 9; ++i) {
            RoutePoint& p = r.pts[r.n_pts++];
            p = RoutePoint{};
            p.time_ds = i * 100;
            p.name_off = i < 9 ? roads[i] : 0;
        }
        RoadLabel out[4];
        const size_t n = longest_roads(r, out, 4);
        CHECK(n == 4);                       // 1, then 2/3/4 each 10% of 90 s
        CHECK(out[0].name_off == 1 && out[0].time_ds == 500);
        CHECK(out[0].point >= 3 && out[0].point <= 6);   // inside the 30 s stretch
        CHECK(out[1].time_ds == 100);
        for (size_t i = 0; i < n; ++i) CHECK(out[i].name_off != 0);
        CHECK(longest_roads(r, out, 2) == 2);

        // A long road and two crumbs under a tenth: only the top two.
        r.n_pts = 0;
        const uint32_t t_ds[] = {0, 1000, 2000, 2050, 2100};
        const uint32_t off[]  = {1, 1, 2, 3, 0};
        for (uint32_t i = 0; i < 5; ++i) {
            RoutePoint& p = r.pts[r.n_pts++];
            p = RoutePoint{};
            p.time_ds = t_ds[i];
            p.name_off = off[i];
        }
        CHECK(longest_roads(r, out, 4) == 2);
        CHECK(out[0].name_off == 1 && out[1].name_off == 2);   // a tie: the first found

        Route empty = m.make();
        CHECK(longest_roads(empty, out, 4) == 0);
    }

    return check::report("route");
}
