// `logic`'s half of the named waypoints: START setting itself, and drop and
// clear by name. The rules themselves are test_waypoint.
#include "check.hpp"
#include "services/logic_tick.hpp"
#include <deque>

namespace {

class Queue final : public services::ICommandSource {
public:
    bool take(core::state::Command& out) override {
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    std::deque<core::state::Command> q;
};

core::state::Gnss fix_at(double lat, double lon, uint32_t utc_ms, uint16_t doy) {
    core::state::Gnss g;
    g.fix = true;
    g.hdop = 1.0f;
    g.lat = lat;
    g.lon = lon;
    g.utc_ms = utc_ms;
    g.year = 2026;
    g.day_of_year = doy;
    g.rx_ms = utc_ms;
    return g;
}

}  // namespace

int main() {
    using core::state::CommandId;
    using core::waypoint::Start;

    CASE("START sets itself at the first good fix, once");
    {
        core::state::Slots slots;
        core::config::Settings settings;
        services::Deps deps;
        deps.slots = &slots;
        deps.settings = &settings;
        services::LogicState s;
        Queue q;
        slots.gnss.publish(core::state::Gnss{});           // no fix yet
        services::logic_tick(deps, s, q, 1000);
        CHECK(!slots.waypoint.get().pts[Start].set);
        slots.gnss.publish(fix_at(42.5, -83.1, 36000000, 286));
        services::logic_tick(deps, s, q, 1020);
        CHECK(slots.waypoint.get().pts[Start].set);
        CHECK(slots.waypoint.get().pts[Start].lat == 42.5);
        slots.gnss.publish(fix_at(42.6, -83.2, 36060000, 286));   // ridden on
        services::logic_tick(deps, s, q, 1040);
        CHECK(slots.waypoint.get().pts[Start].lat == 42.5);       // still home

        // Key off at lunch and back on: a new LogicState, with what keeper
        // had saved. START stays where the ride began.
        services::LogicState after_lunch;
        after_lunch.waypoints = slots.waypoint.get();
        slots.gnss.publish(fix_at(42.7, -83.3, 36000000 + 3600000, 286));
        services::logic_tick(deps, after_lunch, q, 2000);
        CHECK(slots.waypoint.get().pts[Start].lat == 42.5);

        // Tomorrow is a new ride.
        services::LogicState tomorrow;
        tomorrow.waypoints = slots.waypoint.get();
        slots.gnss.publish(fix_at(41.0, -84.0, 30000000, 287));
        services::logic_tick(deps, tomorrow, q, 3000);
        CHECK(slots.waypoint.get().pts[Start].lat == 41.0);
    }

    CASE("drop and clear act on the waypoint named");
    {
        core::state::Slots slots;
        core::config::Settings settings;
        services::Deps deps;
        deps.slots = &slots;
        deps.settings = &settings;
        services::LogicState s;
        Queue q;
        slots.gnss.publish(fix_at(42.5, -83.1, 36000000, 286));
        q.q.emplace_back(CommandId::NavDropWaypoint, int32_t{core::waypoint::Camp});
        services::logic_tick(deps, s, q, 1000);
        CHECK(slots.waypoint.get().pts[core::waypoint::Camp].set);
        CHECK(!slots.waypoint.get().pts[core::waypoint::Truck].set);
        q.q.emplace_back(CommandId::NavClearWaypoint, int32_t{core::waypoint::Camp});
        services::logic_tick(deps, s, q, 1020);
        CHECK(!slots.waypoint.get().pts[core::waypoint::Camp].set);
        CHECK(slots.waypoint.get().pts[Start].set);               // untouched
        q.q.emplace_back(CommandId::NavDropWaypoint, int32_t{9});   // no such one
        services::logic_tick(deps, s, q, 1040);
    }

    return check::report("logic_waypoint");
}
