#include "check.hpp"
#include "core/route/navigator.hpp"

using namespace core::route;

namespace {

constexpr int32_t kLat0 = 360000000, kLon0 = -941600000;
constexpr int32_t kLon100m = 11116;
constexpr int32_t kLat100m = 8993;

RouteStorage<32, 4> mem;

// 1 km due east; `fast` puts all of it on a road judged 65 mph.
Route line(bool fast, uint32_t vflags = 0) {
    Route r = mem.make();
    for (int i = 0; i <= 10; ++i) {
        RoutePoint& p = r.pts[r.n_pts++];
        p = RoutePoint{};
        p.p = Point{kLat0, kLon0 + i * kLon100m};
        p.dist_dm = uint32_t(i * 1000);
        p.time_ds = uint32_t(i * 100);
        p.cap_mph = fast ? 65 : 0;
    }
    r.cues[r.n_cues++] = Cue{10, 10000, 0, 0, CueKind::Arrive, 0};
    r.total_dm = 10000;
    r.total_ds = 1000;
    r.variant_flags = vflags;
    return r;
}

Point east(int m, int north_m = 0) { return {kLat0 + north_m * kLat100m / 100, kLon0 + m * kLon100m / 100}; }
const Point kDest = east(1000);

}  // namespace

int main() {
    CASE("no map: nothing starts; a map makes it idle");
    {
        Navigator n;
        CHECK(n.phase() == Phase::NoMap);
        CHECK(!n.start(kDest, east(0), true));
        n.set_map(true);
        CHECK(n.phase() == Phase::Idle);
        CHECK(!n.start(kDest, east(0), false));    // no fix, no route
        CHECK(n.phase() == Phase::Idle);
    }

    CASE("a slow route: one request on the full graph, then guiding");
    {
        Navigator n;
        n.set_map(true);
        CHECK(n.start(kDest, east(0), true));
        CHECK(n.phase() == Phase::Routing);
        Request q;
        CHECK(n.take_request(q));
        CHECK(!n.take_request(q));                  // once
        CHECK(q.variant_flags == 0);
        CHECK(q.to.lon_e7 == kDest.lon_e7);
        const Route r = line(false);
        CHECK(n.answered(Status::Ok, r, 100));
        CHECK(n.phase() == Phase::Guiding);
        CHECK(n.following());
        const Progress p = n.tick(&r, east(300), true, 200);
        CHECK(p.on);
        CHECK_NEAR(p.remain_dm, 7000, 5);
    }

    CASE("dirt avoided: the request says so");
    {
        Navigator n;
        n.set_map(true);
        n.set_avoid_dirt(true);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        CHECK(q.variant_flags == kNoDirt);
    }

    CASE("the speed limit picks the graph: 55 and 65, with dirt too");
    {
        Navigator n;
        n.set_map(true);
        Request q;
        n.set_max_mph(55);
        n.start(kDest, east(0), true);
        n.take_request(q);
        CHECK(q.variant_flags == limit_flags(55));
        n.set_max_mph(65);
        n.set_avoid_dirt(true);
        n.start(kDest, east(0), true);
        n.take_request(q);
        CHECK(q.variant_flags == (limit_flags(65) | kNoDirt));
        n.set_max_mph(0);
        n.set_avoid_dirt(false);
        n.start(kDest, east(0), true);
        n.take_request(q);
        CHECK(q.variant_flags == 0);
        CHECK(limit_mph(limit_flags(55)) == 55 && limit_flags(0) == 0);
    }

    CASE("a route under the limit is followed at once: nothing is asked");
    {
        Navigator n;
        n.set_map(true);
        n.set_max_mph(55);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        CHECK(n.answered(Status::Ok, line(false, limit_flags(55)), 100));
        CHECK(n.phase() == Phase::Guiding);
        // A reroute keeps the limit.
        const Route r = line(false, limit_flags(55));
        uint32_t t = 0;
        for (; t <= 20000 && n.phase() == Phase::Guiding; t += 1000) n.tick(&r, east(300, 100), true, t);
        CHECK(n.phase() == Phase::Rerouting);
        CHECK(n.take_request(q));
        CHECK(q.variant_flags == limit_flags(55));
    }

    CASE("nothing connects under the limit: told, and may route at any speed");
    {
        Navigator n;
        n.set_map(true);
        n.set_max_mph(55);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        RouteStorage<2, 2> e;
        Route none = e.make();
        CHECK(!n.answered(Status::NoRoute, none, 10));
        CHECK(n.phase() == Phase::NoSlowRoute);
        CHECK(!n.following());
        n.any_speed();
        CHECK(n.phase() == Phase::Routing);
        CHECK(n.take_request(q));
        CHECK(q.variant_flags == 0);
        CHECK(n.answered(Status::Ok, line(true), 20));
        CHECK(n.phase() == Phase::Guiding);
        // Lifted for this destination's reroutes too.
        const Route r = line(true);
        uint32_t t = 0;
        for (; t <= 20000 && n.phase() == Phase::Guiding; t += 1000) n.tick(&r, east(300, 100), true, t);
        CHECK(n.take_request(q));
        CHECK(q.variant_flags == 0);
        // A new destination puts it back.
        n.start(kDest, east(0), true);
        n.take_request(q);
        CHECK(q.variant_flags == limit_flags(55));
    }

    CASE("no route at all with no limit set is Failed, not NoSlowRoute");
    {
        Navigator n;
        n.set_map(true);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        RouteStorage<2, 2> e;
        Route none = e.make();
        CHECK(!n.answered(Status::NoRoute, none, 10));
        CHECK(n.phase() == Phase::Failed);
    }

    CASE("under 55 but on a faster road: routed along it, then under the limit once off it");
    {
        Navigator n;
        n.set_map(true);
        n.set_max_mph(55);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        const Route slow = line(false, limit_flags(55));
        n.answered(Status::Ok, slow, 10);
        CHECK(n.phase() == Phase::Guiding);
        // Off onto the highway, out of reach of any slow road.
        uint32_t t = 20000;
        for (int i = 0; i < 30 && n.phase() == Phase::Guiding; ++i, t += 1000)
            n.tick(&slow, east(300, 100), true, t);
        CHECK(n.phase() == Phase::Rerouting);
        CHECK(n.take_request(q));
        CHECK(q.variant_flags == limit_flags(55));
        CHECK(!n.answered(Status::NoStart, slow, t));
        CHECK(n.phase() == Phase::Rerouting);       // straight away, not after the gap
        CHECK(n.take_request(q));
        CHECK(q.variant_flags == 0);                // this once, no limit
        // The detour: on the highway for 500 m, then off it.
        Route detour = line(true);
        for (uint32_t i = 5; i < detour.n_pts; ++i) detour.pts[i].cap_mph = 0;
        CHECK(n.answered(Status::Ok, detour, t));
        CHECK(n.phase() == Phase::Guiding);
        n.tick(&detour, east(200), true, t += 1000);
        CHECK(n.phase() == Phase::Guiding);         // still on it
        CHECK(!n.take_request(q));
        n.tick(&detour, east(700), true, t += 1000);
        CHECK(n.phase() == Phase::Rerouting);       // off it: under the limit again
        CHECK(n.take_request(q));
        CHECK(q.variant_flags == limit_flags(55));
    }

    CASE("a road over 55 but not 65 is no detour under a 65 limit");
    {
        Navigator n;
        n.set_map(true);
        n.set_max_mph(65);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        const Route r = line(true, limit_flags(65));          // judged 65: not over 65
        n.answered(Status::Ok, r, 0);
        n.tick(&r, east(200), true, 1000);
        CHECK(n.phase() == Phase::Guiding);
        CHECK(!n.take_request(q));
    }

    CASE("a failed reroute keeps the old route and tries again after the gap");
    {
        Navigator n;
        n.set_map(true);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        const Route r = line(false);
        n.answered(Status::Ok, r, 0);
        uint32_t t = 0;
        for (; n.phase() == Phase::Guiding; t += 1000) n.tick(&r, east(300, 100), true, t);
        n.take_request(q);
        CHECK(!n.answered(Status::ReadError, r, t));
        CHECK(n.phase() == Phase::Guiding);
        CHECK(n.following());
        n.tick(&r, east(300, 100), true, t + 1000);
        CHECK(n.phase() == Phase::Guiding);         // within the 10 s gap
        n.tick(&r, east(300, 100), true, t + 11000);
        CHECK(n.phase() == Phase::Rerouting);
    }

    CASE("arriving, then idle after the hold");
    {
        Navigator n;
        n.set_map(true);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        const Route r = line(false);
        n.answered(Status::Ok, r, 0);
        n.tick(&r, east(990), true, 1000);
        CHECK(n.phase() == Phase::Arrived);
        n.tick(&r, east(990), true, 20000);
        CHECK(n.phase() == Phase::Arrived);
        n.tick(&r, east(990), true, 31001);
        CHECK(n.phase() == Phase::Idle);
        CHECK(!n.following());
    }

    CASE("cancel while a request is out: its answer is ignored");
    {
        Navigator n;
        n.set_map(true);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        n.cancel();
        CHECK(!n.answered(Status::Ok, line(false), 0));
        CHECK(n.phase() == Phase::Idle);
        // A new destination while the old answer is out: only the new one counts.
        n.start(kDest, east(0), true);
        n.take_request(q);
        n.start(east(500), east(0), true);
        CHECK(!n.answered(Status::Ok, line(false), 0));
        CHECK(n.take_request(q));
        CHECK(q.to.lon_e7 == east(500).lon_e7);
    }

    CASE("no route at all fails with the reason; the map going away is NoMap");
    {
        Navigator n;
        n.set_map(true);
        n.start(kDest, east(0), true);
        Request q;
        n.take_request(q);
        RouteStorage<2, 2> e;
        Route none = e.make();
        CHECK(!n.answered(Status::NoDest, none, 0));
        CHECK(n.phase() == Phase::Failed);
        CHECK(n.status() == Status::NoDest);
        n.start(kDest, east(0), true);
        n.take_request(q);
        n.answered(Status::NoMap, none, 0);
        CHECK(n.phase() == Phase::NoMap);
    }

    return check::report("route_navigator");
}
