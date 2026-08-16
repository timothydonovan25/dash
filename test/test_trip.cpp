#include "check.hpp"
#include "core/trip/trip.hpp"

using namespace core::trip;

namespace {
// Ride at a constant speed for a whole number of seconds, at the 50 Hz the
// logic task runs at.
void ride(Computer& c, const FuelModel& fm, float mph, int seconds,
          uint32_t& t_ms, bool engine_on = true, float tps = 20.0f) {
    for (int i = 0; i < seconds * 50; ++i) {
        t_ms += 20;
        Input in;
        in.speed_mph = mph;
        in.engine_on = engine_on;
        in.tps_pct   = tps;
        in.t_ms      = t_ms;
        c.update(in, fm);
    }
}
}  // namespace

int main() {
    const FuelModel fm;

    CASE("distance accumulates at the speed given");
    {
        Computer c;
        uint32_t t = 0;
        ride(c, fm, 60.0f, 60, t);            // 60 mph for one minute
        const auto p = c.publish();
        CHECK_NEAR(p.odo_mi, 1.0, 0.002);
        CHECK_NEAR(p.trip_a_mi, 1.0, 0.002);
        CHECK_NEAR(p.trip_b_mi, 1.0, 0.002);
        CHECK_NEAR(p.top_mph, 60.0, 1e-3);
    }

    CASE("the odometer still counts at 100,000 miles");
    {
        // The reason the accumulator is a double. A float odometer at
        // 100,000 mi has a resolution of about 0.0078 mi, and one 20 ms tick
        // at 50 mph is 0.00028 mi — every increment rounds to nothing and
        // the odometer freezes forever. Ten minutes at 50 mph is 8.33 mi and
        // must show up.
        Computer c;
        c.restore(100000.0, 500.0);
        uint32_t t = 0;
        ride(c, fm, 50.0f, 600, t);
        CHECK_NEAR(c.odo_mi(), 100008.333, 0.02);
        // The published float cannot hold that precision, and does not have
        // to — it only has to be right to the tenth of a mile that is drawn.
        CHECK_NEAR(c.publish().odo_mi, 100008.3, 1.0);
    }

    CASE("engine hours only run while the engine does");
    {
        Computer c;
        uint32_t t = 0;
        ride(c, fm, 0.0f, 3600, t, true);      // an hour idling
        CHECK_NEAR(c.engine_hours(), 1.0, 1e-3);
        CHECK_NEAR(c.publish().odo_mi, 0.0, 1e-6);   // stationary adds no miles
        ride(c, fm, 30.0f, 3600, t, false);    // an hour rolling, engine off
        CHECK_NEAR(c.engine_hours(), 1.0, 1e-3);
        CHECK_NEAR(c.publish().odo_mi, 30.0, 0.05);
    }

    CASE("fuel burn follows the throttle");
    {
        CHECK_NEAR(fm.gph_at(0.0f), fm.idle_gph, 1e-6);
        CHECK_NEAR(fm.gph_at(100.0f), fm.wot_gph, 1e-6);
        CHECK_NEAR(fm.gph_at(50.0f), (fm.idle_gph + fm.wot_gph) / 2.0f, 1e-5);
        CHECK_NEAR(fm.gph_at(-10.0f), fm.idle_gph, 1e-6);   // clamped
        CHECK_NEAR(fm.gph_at(999.0f), fm.wot_gph, 1e-6);

        Computer c;
        uint32_t t = 0;
        ride(c, fm, 40.0f, 3600, t, true, 100.0f);
        CHECK_NEAR(c.publish().fuel_used_gal, fm.wot_gph, 0.005);
    }

    CASE("a stalled tick is dropped rather than charged as distance");
    {
        // The logic task missing a deadline must not put miles on the bike.
        // Without the guard, one 30 s gap at 60 mph silently adds half a
        // mile to an odometer that is meant to be permanent.
        Computer c;
        uint32_t t = 0;
        ride(c, fm, 60.0f, 1, t);
        const double after_one_second = c.odo_mi();
        Input in;
        in.speed_mph = 60.0f;
        in.engine_on = true;
        in.t_ms = t + 30000;                  // a 30 s hole
        c.update(in, fm);
        CHECK_NEAR(c.odo_mi(), after_one_second, 1e-9);
        // ...and the clock picks up again from the new time, not the old one.
        t = in.t_ms;
        ride(c, fm, 60.0f, 60, t);
        CHECK_NEAR(c.odo_mi(), after_one_second + 1.0, 0.002);
    }

    CASE("the first update only sets the timebase");
    {
        Computer c;
        Input in;
        in.speed_mph = 100.0f;
        in.t_ms = 5000;                        // boot at a non-zero clock
        c.update(in, fm);
        CHECK_NEAR(c.odo_mi(), 0.0, 1e-12);
    }

    CASE("trip meters reset independently and the odometer never does");
    {
        Computer c;
        uint32_t t = 0;
        ride(c, fm, 60.0f, 60, t);
        c.reset_trip_a();
        auto p = c.publish();
        CHECK_NEAR(p.trip_a_mi, 0.0, 1e-9);
        CHECK_NEAR(p.trip_b_mi, 1.0, 0.002);
        CHECK_NEAR(p.odo_mi, 1.0, 0.002);
        CHECK_NEAR(p.top_mph, 0.0, 1e-9);      // top speed belongs to trip A

        ride(c, fm, 30.0f, 60, t);
        c.reset_trip_b();
        p = c.publish();
        CHECK_NEAR(p.trip_a_mi, 0.5, 0.002);
        CHECK_NEAR(p.trip_b_mi, 0.0, 1e-9);
        CHECK_NEAR(p.odo_mi, 1.5, 0.003);
        CHECK_NEAR(p.top_mph, 30.0, 1e-3);
    }

    CASE("restore puts back what the keeper saved");
    {
        Computer c;
        c.restore(12345.6, 789.0);
        const auto p = c.publish();
        CHECK_NEAR(p.odo_mi, 12345.6, 0.01);
        CHECK_NEAR(p.eng_hours, 789.0, 0.01);
        CHECK_NEAR(p.trip_a_mi, 0.0, 1e-9);    // trips are not life data
    }

    return check::report("trip");
}
