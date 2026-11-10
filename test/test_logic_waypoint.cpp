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

    CASE("totals that could not be read back are marked, so nobody saves zero over them");
    {
        struct Nvs final : hal::IStorage {
            hal::Status odo = hal::Status::Ok;
            hal::Status init() override { return hal::Status::Ok; }
            hal::Status read_f32(const char*, uint16_t, float&) override { return hal::Status::NotPresent; }
            hal::Status write_f32(const char*, uint16_t, float) override { return hal::Status::Ok; }
            hal::Status read_f64(const char*, uint16_t key, double&) override {
                return key == hal::kKeyOdometerMi ? odo : hal::Status::NotPresent;
            }
            hal::Status write_f64(const char*, uint16_t, double) override { return hal::Status::Ok; }
            hal::Status commit() override { return hal::Status::Ok; }
        };
        for (const hal::Status st : {hal::Status::Ok, hal::Status::NotPresent, hal::Status::IoError}) {
            Nvs nvs;
            nvs.odo = st;
            core::state::Slots slots;
            services::Deps deps;
            deps.slots = &slots;
            deps.storage = &nvs;
            services::LogicState s;
            services::logic_restore(deps, s);
            CHECK(deps.totals_unread.load() == (st == hal::Status::IoError));
        }
    }

    CASE("the laptop's odo and hours set the totals; mark done restarts the service interval");
    {
        core::state::Slots slots;
        core::config::Settings settings;
        settings.service_every_h = 10.0f;
        services::Deps deps;
        deps.slots = &slots;
        deps.settings = &settings;
        deps.totals_unread.store(true);
        services::LogicState s;
        Queue q;
        q.q.emplace_back(CommandId::OdoSet, 1234.5f);
        services::logic_tick(deps, s, q, 1000);
        CHECK(deps.totals_unread.load());                  // hours not given yet
        q.q.emplace_back(CommandId::EngHoursSet, 87.0f);
        q.q.emplace_back(CommandId::OdoSet, -5.0f);        // refused
        services::logic_tick(deps, s, q, 1020);
        CHECK(!deps.totals_unread.load());
        CHECK_NEAR(slots.trip.get().odo_mi, 1234.5, 0.01);
        CHECK_NEAR(slots.trip.get().eng_hours, 87.0, 0.01);
        CHECK(slots.trip.get().service_due);               // 87 h past a service at 0
        q.q.emplace_back(CommandId::ServiceDone);
        services::logic_tick(deps, s, q, 1040);
        CHECK(!slots.trip.get().service_due);
        CHECK_NEAR(settings.service_at_h, 87.0, 0.01);
    }

    return check::report("logic_waypoint");
}
