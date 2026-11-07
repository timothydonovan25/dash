#include "check.hpp"
#include "core/route/follow.hpp"

using namespace core::route;

namespace {

// 1 km east, then 1 km north, a point every 100 m, a left cue at the corner.
constexpr int32_t kLat0 = 360000000, kLon0 = -941600000;
constexpr int32_t kLat100m = 8993;          // 100 m of latitude, e7
constexpr int32_t kLon100m = 11116;         // 100 m of longitude at 36 N, e7

RouteStorage<64, 8> mem;

Route make_route() {
    Route r = mem.make();
    for (int i = 0; i <= 20; ++i) {
        RoutePoint& p = r.pts[r.n_pts++];
        p = RoutePoint{};
        p.p = i <= 10 ? Point{kLat0, kLon0 + i * kLon100m}
                      : Point{kLat0 + (i - 10) * kLat100m, kLon0 + 10 * kLon100m};
        p.dist_dm = uint32_t(i * 1000);
        p.time_ds = uint32_t(i * 100);       // 10 s per 100 m
        p.name_off = i < 10 ? 1 : 2;
        p.mph = i < 10 ? 45 : 0;
        p.flags = i < 10 ? 0 : kAttrFast;
    }
    r.cues[r.n_cues++] = Cue{10, 10000, 2, -90, CueKind::Left, kAttrFast};
    r.cues[r.n_cues++] = Cue{20, 20000, 2, 0, CueKind::Arrive, 0};
    r.total_dm = 20000;
    r.total_ds = 2000;
    return r;
}

Point east(int m, int north_m = 0) {
    return {kLat0 + north_m * kLat100m / 100, kLon0 + m * kLon100m / 100};
}

}  // namespace

int main() {
    const Route r = make_route();

    CASE("on the first road: distance to the left turn and what is left");
    {
        Follower f;
        const Progress p = f.update(r, east(350), true, 0);
        CHECK(p.on && !p.off_route && !p.arrived);
        CHECK_NEAR(p.along_dm, 3500, 5);
        CHECK(p.cue == 0);
        CHECK_NEAR(p.to_cue_dm, 6500, 5);
        CHECK_NEAR(p.remain_dm, 16500, 5);
        CHECK_NEAR(p.remain_ds, 1650, 2);
        CHECK(p.name_off == 1 && p.mph == 45 && !(p.flags & kAttrFast));
    }

    CASE("past the turn: the next cue is the arrival, and the road is the fast one");
    {
        Follower f;
        f.update(r, east(900), true, 0);
        const Progress p = f.update(r, east(1000, 300), true, 1000);
        CHECK(p.cue == 1);
        CHECK_NEAR(p.to_cue_dm, 7000, 5);
        CHECK(p.flags & kAttrFast);
    }

    CASE("30 m off is still on the route; 80 m off for 5 s asks for a reroute once");
    {
        Follower f;
        CHECK(!f.update(r, east(200, -30), true, 0).off_route);
        Progress p = f.update(r, east(250, -80), true, 1000);
        CHECK(p.off_route && !p.reroute);
        CHECK_NEAR(p.xtrack_dm, 800, 10);
        p = f.update(r, east(260, -80), true, 5000);
        CHECK(!p.reroute);
        p = f.update(r, east(270, -80), true, 6000);
        CHECK(p.reroute);                       // 5 s after it left
        p = f.update(r, east(280, -80), true, 7000);
        CHECK(p.off_route && !p.reroute);       // asked once
        p = f.update(r, east(300, -10), true, 8000);
        CHECK(!p.off_route);
        f.update(r, east(310, -80), true, 9000);
        CHECK(f.update(r, east(320, -80), true, 14000).reroute);   // a new episode asks again
    }

    CASE("losing the fix holds the answer and is not leaving the route");
    {
        Follower f;
        f.update(r, east(400), true, 0);
        Progress p = f.update(r, Point{0, 0}, false, 20000);
        CHECK(!p.on && !p.reroute && !p.off_route);
        CHECK_NEAR(p.along_dm, 4000, 5);
    }

    CASE("arriving latches");
    {
        Follower f;
        f.update(r, east(1000, 500), true, 0);
        CHECK(f.update(r, east(1000, 980), true, 1000).arrived);
        CHECK(f.update(r, east(1000, 900), true, 2000).arrived);
        f.reset();
        CHECK(!f.update(r, east(1000, 900), true, 3000).arrived);
    }

    CASE("no route, no progress");
    {
        Follower f;
        Route empty = mem.make();
        CHECK(!f.update(empty, east(0), true, 0).on);
    }

    return check::report("route_follow");
}
