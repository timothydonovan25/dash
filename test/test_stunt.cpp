// Wheelies and stoppies: what counts, what doesn't (a hill, a jump, a
// twitch), and how they score.
#include "check.hpp"
#include "core/stunt/stunt.hpp"

#include <cmath>
#include <cstring>
#include <functional>

using core::stunt::Config;
using core::stunt::Input;
using core::stunt::Scorer;
namespace st = core::stunt;

namespace {

constexpr double kDeg2Rad = 3.14159265358979 / 180.0;
constexpr double kMpsPerMph = 0.44704;

// One instant of a synthetic ride: what the bike is doing, from which the
// sensors' readings are worked out the way physics would make them.
struct Moment {
    double pitch_deg = 0.0;   // the bike against level
    double slope_deg = 0.0;   // the path against level (a hill)
    double accel_g = 0.0;     // along the path
    double mph = 20.0;
    bool   airborne = false;
    double extra_vert_g = 0.0;   // a landing's spike
};

struct Ride {
    Scorer s;
    Config c;
    uint32_t t = 1000;
    double alt_m = 100.0;

    // `ms` of riding on 20 ms passes, the logic task's rate.
    double last_slope = 0.0;

    void run(uint32_t ms, const std::function<Moment(double)>& at) {
        for (uint32_t k = 0; k < ms; k += 20) {
            const Moment m = at(k / 1000.0);
            // World specific force: the path acceleration, the push that
            // bends the path into a slope (v times its turn rate, along the
            // path's normal), plus 1 g up.
            const double sl = m.slope_deg * kDeg2Rad, th = m.pitch_deg * kDeg2Rad;
            const double turn = (sl - last_slope) / 0.020;          // rad/s
            last_slope = sl;
            const double an = m.mph * kMpsPerMph * turn / 9.80665;  // g
            const double fx = m.accel_g * std::cos(sl) - an * std::sin(sl);
            const double fy = m.accel_g * std::sin(sl) + an * std::cos(sl) + 1.0;
            Input in;
            in.t_ms = t;
            in.pitch_deg = static_cast<float>(m.pitch_deg);
            in.g_lon = static_cast<float>(fx * std::cos(th) + fy * std::sin(th));
            in.g_vert = static_cast<float>(-fx * std::sin(th) + fy * std::cos(th) +
                                           m.extra_vert_g);
            if (m.airborne) in.g_lon = in.g_vert = 0.0f;
            in.airborne = m.airborne;
            in.imu_ok = true;
            in.mph = static_cast<float>(m.mph);
            in.speed_valid = true;
            in.gps_fix = true;
            in.gps_alt_m = static_cast<float>(alt_m);
            s.update(in, c);
            alt_m += m.mph * kMpsPerMph * std::sin(sl) * 0.020;
            t += 20;
        }
    }
    void cruise(uint32_t ms) { run(ms, [](double) { return Moment{}; }); }
    // Up to `deg` over 0.2 s, held, down over 0.2 s: `ms` long in all.
    void wheelie(uint32_t ms, double deg, double mph = 20.0) {
        const double total = ms / 1000.0;
        run(ms, [=](double s) {
            Moment m;
            m.mph = mph;
            const double ramp = 0.2;
            m.pitch_deg = s < ramp ? deg * s / ramp
                        : s > total - ramp ? deg * (total - s) / ramp : deg;
            return m;
        });
    }
    core::state::Stunt out() const { return s.publish(); }
};

}  // namespace

int main() {
    CASE("a 3.5 s wheelie at 30 degrees on the flat is one trick");
    {
        Ride r;
        r.cruise(3000);
        r.wheelie(3500, 30.0);
        r.cruise(1000);
        const auto o = r.out();
        CHECK(o.seq == 1);
        CHECK(o.kind == st::Wheelie);
        // From 20 degrees up to 4 degrees down, on 0.2 s ramps to 30.
        CHECK_NEAR(o.ms / 1000.0, 3.5 - 0.2 * 20.0 / 30.0 - 0.2 * 4.0 / 30.0, 0.06);
        CHECK(std::strcmp(st::grade_name(o.kind, o.grade), "NICE ONE") == 0);
        CHECK_NEAR(o.peak_deg, 30.0, 0.5);
        CHECK(o.points > 300);
        // And by road distance: 20 mph is 29.3 ft/s, over the same time up.
        CHECK_NEAR(o.ft, o.ms / 1000.0 * 20.0 * 1.46667, 1.5);
        CHECK(o.longest_ft == o.ft);
        CHECK(o.total == o.points);
        CHECK(o.tricks == 1);
        CHECK(o.record);
        CHECK(o.live_kind == st::None);
        // Scored after the landing window, so a little after the end.
        CHECK(o.done_ms > 1000 + 3000 + 3300 && o.done_ms <= r.t);
    }

    CASE("riding up a 20 degree hill is not a wheelie");
    {
        Ride r;
        r.cruise(3000);
        // Into the hill over half a second, nose and path together.
        r.run(8000, [](double s) {
            Moment m;
            m.mph = 15.0;
            const double k = s < 0.5 ? s / 0.5 : 1.0;
            m.pitch_deg = m.slope_deg = 20.0 * k;
            return m;
        });
        CHECK(r.out().seq == 0);
        CHECK_NEAR(r.s.ground_deg(), 20.0, 2.0);
        CHECK(std::fabs(r.s.lift_deg()) < 3.0);
        // And a wheelie on the hill is still one: lift over the slope.
        r.run(2500, [](double s) {
            Moment m;
            m.mph = 15.0;
            m.slope_deg = 20.0;
            m.pitch_deg = 20.0 + (s < 0.2 ? 125.0 * s : s > 2.3 ? 125.0 * (2.5 - s) : 25.0);
            return m;
        });
        r.run(1000, [](double) {
            Moment m;
            m.mph = 15.0;
            m.pitch_deg = m.slope_deg = 20.0;
            return m;
        });
        CHECK(r.out().seq == 1);
        CHECK(r.out().kind == st::Wheelie);
        CHECK_NEAR(r.out().peak_deg, 25.0, 3.0);
    }

    CASE("hard acceleration on the flat does not read as a climb");
    {
        Ride r;
        r.cruise(2000);
        r.run(4000, [](double s) {
            Moment m;
            m.accel_g = 0.6;
            m.mph = 10.0 + 13.0 * s;
            return m;
        });
        CHECK(std::fabs(r.s.ground_deg()) < 2.0);
        CHECK(r.out().seq == 0);
    }

    CASE("a twitch under half a second scores nothing and breaks nothing");
    {
        Ride r;
        r.cruise(2000);
        r.wheelie(400, 25.0);
        r.cruise(1000);
        CHECK(r.out().seq == 0);
        CHECK(r.out().total == 0);
    }

    CASE("nose up in the air is a jump, not a wheelie");
    {
        Ride r;
        r.cruise(2000);
        r.run(1500, [](double) {
            Moment m;
            m.airborne = true;
            m.pitch_deg = 25.0;
            return m;
        });
        r.cruise(1000);
        CHECK(r.out().seq == 0);
    }

    CASE("standing still with the front up is not riding");
    {
        Ride r;
        r.run(2000, [](double) { Moment m; m.mph = 0.0; return m; });
        r.run(2000, [](double) { Moment m; m.mph = 0.0; m.pitch_deg = 30.0; return m; });
        CHECK(r.out().seq == 0);
    }

    CASE("a 1.2 s stoppie is a big stoppie");
    {
        Ride r;
        r.cruise(2000);
        r.run(1400, [](double s) {
            Moment m;
            m.mph = 12.0;
            m.accel_g = -0.8;
            m.pitch_deg = s < 0.1 ? -250.0 * s : s > 1.3 ? -250.0 * (1.4 - s) : -25.0;
            return m;
        });
        r.cruise(1000);
        const auto o = r.out();
        CHECK(o.seq == 1);
        CHECK(o.kind == st::Stoppie);
        CHECK(std::strcmp(st::grade_name(o.kind, o.grade), "BIG STOPPIE") == 0);
        CHECK_NEAR(o.peak_deg, 25.0, 0.5);
    }

    CASE("tricks within four seconds chain; a gap starts over");
    {
        Ride r;
        r.cruise(2000);
        r.wheelie(1500, 20.0);
        r.cruise(1500);
        const uint32_t first = r.out().points;
        CHECK(r.out().combo == 1);
        CHECK(r.out().combo_x10 == 15);   // the next one would get 1.5x
        r.wheelie(1500, 20.0);
        r.cruise(1500);
        CHECK(r.out().mult_x10 == 15);
        CHECK(r.out().combo == 2);
        // Same trick, half again: allow for the landing rounding.
        CHECK(r.out().points >= first * 3 / 2 - 2 && r.out().points <= first * 3 / 2 + 2);
        r.cruise(6000);
        CHECK(r.out().combo == 0);
        CHECK(r.out().combo_x10 == 10);
        r.wheelie(1500, 20.0);
        r.cruise(1000);
        CHECK(r.out().mult_x10 == 10);
        CHECK(r.out().best_combo == 2);
        CHECK(r.out().tricks == 3);
    }

    CASE("over the top is a loop-out: no points, and the combo is gone");
    {
        Ride r;
        r.cruise(2000);
        r.wheelie(1500, 20.0);
        r.cruise(1000);
        CHECK(r.out().combo == 1);
        const uint32_t total = r.out().total;
        r.run(1500, [](double s) { Moment m; m.pitch_deg = 60.0 * s; return m; });
        CHECK(r.out().kind == st::Looped);
        CHECK(r.out().points == 0);
        CHECK(r.out().combo == 0);
        CHECK(r.out().total == total);
    }

    CASE("a slammed landing is worth less than a smooth one");
    {
        Ride smooth, slam;
        smooth.cruise(2000);
        smooth.wheelie(2000, 25.0);
        smooth.cruise(1000);
        slam.cruise(2000);
        slam.wheelie(2000, 25.0);
        slam.run(1000, [](double s) {
            Moment m;
            m.extra_vert_g = s < 0.06 ? 2.0 : 0.0;
            return m;
        });
        CHECK(smooth.out().landing == st::Smooth);
        CHECK(slam.out().landing == st::Slammed);
        CHECK(slam.out().points < smooth.out().points);
    }

    CASE("the balance point scores faster than a low wheelie");
    {
        Ride low, bal;
        low.cruise(2000);
        low.wheelie(3000, 20.0);
        low.cruise(1000);
        bal.cruise(2000);
        bal.wheelie(3000, 40.0);
        bal.cruise(1000);
        // Angle alone would be (1 + 30/20) / (1 + 10/20) = 1.67x; the band
        // adds half again.
        CHECK(bal.out().points > low.out().points * 2);
    }

    CASE("while a wheelie is up, it is live; reset starts the session over");
    {
        Ride r;
        r.cruise(2000);
        r.run(1500, [](double s) { Moment m; m.pitch_deg = s < 0.2 ? 150.0 * s : 30.0; return m; });
        const auto live = r.out();
        CHECK(live.live_kind == st::Wheelie);
        CHECK(live.live_ms > 1000);
        CHECK_NEAR(live.live_ft, live.live_ms / 1000.0 * 20.0 * 1.46667, 1.5);
        CHECK(live.live_points > 0.0f);
        CHECK_NEAR(live.live_deg, 30.0, 0.5);
        CHECK(live.live_balance);
        r.cruise(2000);
        CHECK(r.out().total > 0);
        const uint16_t seq = r.out().seq;
        r.s.reset_session();
        CHECK(r.out().total == 0);
        CHECK(r.out().tricks == 0);
        CHECK(r.out().seq == seq);
    }

    return check::report("stunt");
}
