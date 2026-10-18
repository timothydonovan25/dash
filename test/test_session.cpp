// The ride so far: distance, time, top speed and the jumps.
#include "check.hpp"
#include "core/session/session.hpp"

using core::session::Input;
using core::session::Tracker;

int main() {
    CASE("an hour at 30 mph is 30 miles and an hour of riding");
    {
        Tracker t;
        Input in;
        in.speed_mph = 30.0f;
        in.engine_on = true;
        for (uint32_t ms = 0; ms <= 3600u * 1000u; ms += 20) {
            in.t_ms = 1000 + ms;
            t.update(in);
        }
        const core::state::Session s = t.publish();
        CHECK_NEAR(s.dist_mi, 30.0, 0.01);
        CHECK(s.moving_s == 3600 || s.moving_s == 3599);
        CHECK(s.engine_s == 3600 || s.engine_s == 3599);
        CHECK_NEAR(s.top_mph, 30.0, 1e-6);
        CHECK_NEAR(s.avg_mph, 30.0, 0.01);
    }

    CASE("idling and walking it are not riding time");
    {
        Tracker t;
        Input in;
        in.engine_on = true;
        for (uint32_t ms = 0; ms <= 60000; ms += 20) {
            in.t_ms = ms;
            in.speed_mph = ms < 30000 ? 0.0f : 2.0f;
            t.update(in);
        }
        const core::state::Session s = t.publish();
        CHECK(s.moving_s == 0);
        CHECK(s.engine_s >= 59);
        CHECK(s.dist_mi > 0.0f);        // the walk still covered ground
        CHECK(s.avg_mph == 0.0f);       // but there is no riding to average
    }

    CASE("a stalled logic task does not invent a mile");
    {
        Tracker t;
        Input in;
        in.speed_mph = 60.0f;
        in.t_ms = 0;
        t.update(in);
        in.t_ms = 5000;                 // five seconds in one tick
        t.update(in);
        CHECK(t.publish().dist_mi == 0.0f);
    }

    CASE("each landing counts once, and the longest and hardest are kept");
    {
        Tracker t;
        Input in;
        in.imu_ok = true;
        // Already in the slot before this session: not this session's.
        in.land_ms = 500;
        in.airtime_ms = 2000;
        in.land_g = 4.0f;
        in.t_ms = 1000;
        t.update(in);
        CHECK(t.publish().jumps == 0);

        auto land = [&](uint32_t when, uint32_t air, float g) {
            in.land_ms = when;
            in.airtime_ms = air;
            in.land_g = g;
            for (int i = 0; i < 10; ++i) {   // held in the slot, seen many times
                in.t_ms += 20;
                t.update(in);
            }
        };
        land(2000, 800, 2.5f);
        land(5000, 1200, 1.8f);
        land(9000, 400, 3.1f);
        const core::state::Session s = t.publish();
        CHECK(s.jumps == 3);
        CHECK(s.best_air_ms == 1200);
        CHECK_NEAR(s.hardest_land_g, 3.1, 1e-6);
    }

    CASE("a landing the IMU cannot vouch for is not counted");
    {
        Tracker t;
        Input in;
        in.t_ms = 0;
        t.update(in);
        in.imu_ok = false;
        in.land_ms = 100;
        in.airtime_ms = 3000;
        in.t_ms = 20;
        t.update(in);
        CHECK(t.publish().jumps == 0);
        CHECK(t.publish().best_air_ms == 0);
    }

    return check::report("session");
}
