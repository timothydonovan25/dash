// Performance timers: launches, distances and braking, against the closed
// form of a constant acceleration.
#include "check.hpp"
#include "core/perf/perf.hpp"

#include <cmath>

using core::perf::Config;
using core::perf::Input;
using core::perf::Timer;
using M = core::state::Perf;

namespace {

constexpr double kFtPerSPerMph = 5280.0 / 3600.0;
constexpr double kG = 32.174;   // ft/s^2

// Runs `t` for `ms` at `mph(t)` on 20 ms passes from `start`, the logic
// task's rate. Returns the time after.
template <typename F>
uint32_t drive(Timer& t, const Config& c, uint32_t start, uint32_t ms, F mph,
               bool valid = true) {
    for (uint32_t k = 0; k <= ms; k += 20) {
        Input in;
        in.t_ms = start + k;
        in.mph = static_cast<float>(mph(k / 1000.0));
        in.valid = valid;
        t.update(in, c);
    }
    return start + ms;
}

// Standing still, long enough to arm.
uint32_t park(Timer& t, const Config& c, uint32_t start) {
    return drive(t, c, start, 2000, [](double) { return 0.0; });
}

}  // namespace

int main() {
    CASE("a constant 0.5 g launch times to the closed form");
    {
        Timer t;
        const Config c;
        uint32_t now = park(t, c, 1000);
        CHECK(t.publish(false).phase == 1);   // armed
        const double a = 0.5 * kG;            // ft/s^2
        const auto mph = [&](double s) { return a * s / kFtPerSPerMph; };
        drive(t, c, now + 20, 12000, mph);
        const M p = t.publish(false);

        // The clock starts where the speed passes stop_mph, not at zero.
        const double ts = c.stop_mph * kFtPerSPerMph / a;
        const double ds = 0.5 * a * ts * ts;
        const auto to = [&](double v) { return v * kFtPerSPerMph / a - ts; };
        const auto at_ft = [&](double ft) { return std::sqrt(2.0 * (ft + ds) / a) - ts; };

        CHECK_NEAR(p.last[M::To1].ms / 1000.0, to(30.0), 0.025);
        CHECK_NEAR(p.last[M::To2].ms / 1000.0, to(60.0), 0.025);
        CHECK_NEAR(p.last[M::Ft60].ms / 1000.0, at_ft(60.0), 0.025);
        CHECK_NEAR(p.last[M::Ft330].ms / 1000.0, at_ft(330.0), 0.025);
        CHECK_NEAR(p.last[M::Eighth].ms / 1000.0, at_ft(660.0), 0.025);
        // Trap speed at the eighth: a * t, in mph.
        CHECK_NEAR(p.last[M::Eighth].x, a * (at_ft(660.0) + ts) / kFtPerSPerMph, 0.3);
        // First runs are bests, and each one was a result.
        CHECK(p.best[M::To2].ms == p.last[M::To2].ms);
        CHECK(p.seq == 5);
        CHECK(p.phase == 0);   // every launch threshold done
    }

    CASE("pulling away without standing still first is not a run");
    {
        Timer t;
        const Config c;
        uint32_t now = drive(t, c, 1000, 400, [](double) { return 0.0; });
        drive(t, c, now + 20, 8000, [](double s) { return 20.0 * s; });
        const M p = t.publish(false);
        CHECK(p.seq == 0);
        CHECK(p.last[M::To1].ms == 0);
    }

    CASE("rolling off before 60 keeps what was reached and drops the rest");
    {
        Timer t;
        const Config c;
        uint32_t now = park(t, c, 1000);
        // 0 to 40 over 4 s, then easing back down.
        drive(t, c, now + 20, 8000, [](double s) {
            return s < 4.0 ? 10.0 * s : 40.0 - 5.0 * (s - 4.0);
        });
        const M p = t.publish(false);
        CHECK(p.last[M::To1].ms > 0);
        CHECK(p.last[M::To2].ms == 0);
        CHECK(p.last[M::Ft60].ms > 0);
        CHECK(p.phase != 2);   // the launch is over
    }

    CASE("a 0.8 g stop from 70 gives 60-0 and 30-0 by the closed form");
    {
        Timer t;
        const Config c;
        const double a = 0.8 * kG;
        uint32_t now = drive(t, c, 1000, 1000, [](double) { return 70.0; });
        drive(t, c, now + 20, 6000, [&](double s) {
            const double v = 70.0 - a * s / kFtPerSPerMph;
            return v > 0.0 ? v : 0.0;
        });
        const M p = t.publish(false);
        const auto dist = [&](double mph) {
            const double v = mph * kFtPerSPerMph;
            return v * v / (2.0 * a);
        };
        CHECK_NEAR(p.last[M::Brake2].x, dist(60.0), 1.0);
        CHECK_NEAR(p.last[M::Brake1].x, dist(30.0), 0.5);
        CHECK_NEAR(p.last[M::Brake2].ms / 1000.0, 60.0 * kFtPerSPerMph / a, 0.03);
        CHECK(p.best[M::Brake2].x == p.last[M::Brake2].x);
        // Two results, the last of them the 60-0 (both end at the same stop).
        CHECK(p.seq == 2);
    }

    CASE("speeding back up abandons a stop");
    {
        Timer t;
        const Config c;
        uint32_t now = drive(t, c, 1000, 1000, [](double) { return 35.0; });
        // Down to 25, then back up to 40, then a real stop from there.
        now = drive(t, c, now + 20, 4000, [](double s) {
            return s < 1.0 ? 35.0 - 10.0 * s : 25.0 + 7.5 * (s - 1.0);
        });
        CHECK(t.publish(false).phase != 3);
        CHECK(t.publish(false).seq == 0);
        drive(t, c, now + 20, 8000, [](double s) {
            const double v = 55.0 - 15.0 * s;
            return v > 0.0 ? v : 0.0;
        });
        const M p = t.publish(false);
        // 30 mph at 15 mph/s, to the stop at stop_mph: 29.5/15 s, and about
        // 22 ft/s average over it.
        CHECK_NEAR(p.last[M::Brake1].ms / 1000.0, (30.0 - c.stop_mph) / 15.0, 0.03);
        CHECK_NEAR(p.last[M::Brake1].x, 44.0, 1.0);
    }

    CASE("slowing and riding on is not a stop, however long until the next one");
    {
        // 65 down to 45 over 2 s, then on at 45 for a minute, then a real
        // 0.8 g stop. The 60-0 was left open the whole minute and scored
        // the stop at the end as a braking distance of most of a mile.
        Timer t;
        const Config c;
        uint32_t now = drive(t, c, 1000, 1000, [](double) { return 65.0; });
        now = drive(t, c, now + 20, 2000, [](double s) { return 65.0 - 10.0 * s; });
        now = drive(t, c, now + 20, 60000, [](double) { return 45.0; });
        CHECK(t.publish(false).phase != 3);
        const double a = 0.8 * kG;
        drive(t, c, now + 20, 4000, [&](double s) {
            const double v = 45.0 - a * s / kFtPerSPerMph;
            return v > 0.0 ? v : 0.0;
        });
        const M p = t.publish(false);
        CHECK(p.last[M::Brake2].ms == 0);           // no 60-0: it never stopped from 60
        CHECK(p.last[M::Brake1].ms != 0);           // the 30-0 of the real stop
        CHECK(p.last[M::Brake1].x < 50.0f);
    }

    CASE("easing off below the start speed and riding on abandons the stop");
    {
        // 35 down to 20, back up to 28 (still under 30 + regain), on at 28,
        // then a stop: the 30-0 is abandoned at the climb from 20.
        Timer t;
        const Config c;
        uint32_t now = drive(t, c, 1000, 1000, [](double) { return 35.0; });
        now = drive(t, c, now + 20, 3000, [](double s) {
            return s < 1.5 ? 35.0 - 10.0 * s : 20.0 + 8.0 * (s - 1.5) / 1.5;
        });
        CHECK(t.publish(false).phase != 3);
        drive(t, c, now + 20, 3000, [](double s) {
            const double v = 28.0 - 14.0 * s;
            return v > 0.0 ? v : 0.0;
        });
        CHECK(t.publish(false).last[M::Brake1].ms == 0);
    }

    CASE("a speed estimate that drops out abandons the run");
    {
        Timer t;
        const Config c;
        uint32_t now = park(t, c, 1000);
        now = drive(t, c, now + 20, 1000, [](double s) { return 10.0 * s; });
        CHECK(t.publish(false).phase == 2);
        now = drive(t, c, now + 20, 100, [](double) { return 12.0; }, false);
        CHECK(t.publish(false).phase == 0);
        drive(t, c, now + 20, 5000, [](double s) { return 12.0 + 10.0 * s; });
        CHECK(t.publish(false).last[M::To1].ms == 0);
    }

    CASE("a best is only replaced by a better run, and E forgets them");
    {
        Timer t;
        const Config c;
        const auto launch = [&](uint32_t start, double mph_per_s) {
            uint32_t now = park(t, c, start);
            return drive(t, c, now + 20, 6000, [&](double s) { return mph_per_s * s; });
        };
        uint32_t now = launch(1000, 15.0);       // 0-30 in 2 s
        const uint32_t first = t.publish(false).best[M::To1].ms;
        now = launch(now + 20, 10.0);            // 3 s: slower
        M p = t.publish(false);
        CHECK(p.best[M::To1].ms == first);
        CHECK(p.last[M::To1].ms > first);
        CHECK(!p.event_best || p.event != M::To1);
        now = launch(now + 20, 20.0);            // 1.5 s: faster
        p = t.publish(false);
        CHECK(p.best[M::To1].ms < first);
        t.reset_bests();
        p = t.publish(false);
        CHECK(p.best[M::To1].ms == 0);
        CHECK(p.last[M::To1].ms > 0);
    }

    CASE("metric riders get 0-50 and 0-100 km/h");
    {
        const Config c = core::perf::for_units(true);
        CHECK_NEAR(c.to_mph[0], 31.07, 0.01);
        CHECK_NEAR(c.to_mph[1], 62.14, 0.01);
        CHECK_NEAR(c.brake_mph[1], 62.14, 0.01);
        Timer t;
        uint32_t now = park(t, c, 1000);
        drive(t, c, now + 20, 8000, [](double s) { return 10.0 * s; });
        const M p = t.publish(true);
        CHECK(p.metric);
        CHECK_NEAR(p.last[M::To1].ms / 1000.0, (31.07 - 0.5) / 10.0, 0.025);
    }

    return check::report("perf");
}
