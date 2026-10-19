#include "check.hpp"
#include <algorithm>
#include "core/attitude/attitude.hpp"
#include <cmath>
#include <vector>

using namespace core::attitude;

namespace {
constexpr double kPi = 3.14159265358979;
constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
constexpr double kG = 32.174;
constexpr double kFtPerSPerMph = 1.46666667;
constexpr double kD = kPi / 180.0;

// What a sensor reads, built from the truth with its own rotation maths so
// the filter's is not marking its own homework. Frame: x forward, y left,
// z up, right-handed (the convention in attitude.hpp).
struct V { double x, y, z; };
struct M { double m[3][3]; };
V operator*(const M& a, V v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}
M T(const M& a) {
    M t{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t.m[i][j] = a.m[j][i];
    return t;
}
// Axes of a frame turned by `deg` about unit axis u, as columns.
M turn(V u, double deg) {
    const double c = std::cos(deg * kD), s = std::sin(deg * kD), k = 1 - c;
    return {{{c + u.x * u.x * k,       u.x * u.y * k - u.z * s, u.x * u.z * k + u.y * s},
             {u.y * u.x * k + u.z * s, c + u.y * u.y * k,       u.y * u.z * k - u.x * s},
             {u.z * u.x * k - u.y * s, u.z * u.y * k + u.x * s, c + u.z * u.z * k}}};
}

struct Truth {
    double lean_deg = 0, pitch_deg = 0;     // > 0 right, > 0 nose up
    double roll_dps = 0, pitch_dps = 0;     // the frame's Euler rates
    double yaw_dps = 0;                     // about the world's vertical, > 0 right
    V      f{0, 0, 1};                      // specific force, the frame's axes
    double steer_deg = 0;                   // > 0 right
    double rake_deg = 27.0;
    double mount_roll_deg = 0, mount_pitch_deg = 0;
    double mph = 0;
};

Sample sense(const Truth& t, uint32_t t_ms) {
    // Euler rates to the frame's body rates (roll right, nose up, yaw right).
    const double ph = t.lean_deg * kD, th = t.pitch_deg * kD;
    const double p = t.roll_dps * kD - std::sin(th) * t.yaw_dps * kD;
    const double q = std::cos(ph) * t.pitch_dps * kD + std::sin(ph) * std::cos(th) * t.yaw_dps * kD;
    const double r = -std::sin(ph) * t.pitch_dps * kD + std::cos(ph) * std::cos(th) * t.yaw_dps * kD;
    const V w_frame{p / kD, -q / kD, -r / kD};           // into x-fwd, y-left, z-up

    // Frame -> bars: a right steer is a negative turn about the head tube.
    const M bars = turn({-std::sin(t.rake_deg * kD), 0, std::cos(t.rake_deg * kD)},
                        -t.steer_deg);
    // Bars -> board: rolled right, then pitched nose up (about -y).
    const M mount_axes = turn({0, -1, 0}, t.mount_pitch_deg);
    const M roll_axes  = turn({1, 0, 0}, t.mount_roll_deg);
    // Components in a turned frame are the transpose times the vector.
    auto to_sensor = [&](V v) { return T(roll_axes) * (T(mount_axes) * (T(bars) * v)); };
    const V w = to_sensor(w_frame), f = to_sensor(t.f);

    Sample s;
    s.gx_dps = static_cast<float>(w.x);
    s.gy_dps = static_cast<float>(w.y);
    s.gz_dps = static_cast<float>(w.z);
    s.ax_g = static_cast<float>(f.x);
    s.ay_g = static_cast<float>(f.y);
    s.az_g = static_cast<float>(f.z);
    s.speed_mph = static_cast<float>(t.mph);
    s.t_ms = t_ms;
    return s;
}

// A steady, coordinated turn at `lean_deg`: Ω = g·tan φ / v, the resultant
// straight down through the bike, so the accelerometer sees no lean at all.
// With `on_bars`, the bars are turned as far as that corner needs.
Truth steady(double lean_deg, double mph, bool on_bars, double wheelbase_ft = 4.63) {
    Truth t;
    const double v = mph * kFtPerSPerMph;
    t.lean_deg = lean_deg;
    t.yaw_dps = std::tan(lean_deg * kD) * kG / v / kD;
    t.f = {0, 0, 1.0 / std::cos(lean_deg * kD)};
    t.mph = mph;
    if (on_bars) {
        // Front wheel on the circle: tan Δ = L/R, and tan Δ = tan δ cos ε / cos φ.
        const double ground = wheelbase_ft * t.yaw_dps * kD / v;
        t.steer_deg = std::atan(ground * std::cos(lean_deg * kD) /
                                std::cos(t.rake_deg * kD)) / kD;
    }
    return t;
}

// Roll into `end` over half a second from upright, then hold for `hold_ms`.
template <class F>
void ride_into(Filter& f, const Config& cfg, const Truth& end, uint32_t hold_ms, F&& each) {
    uint32_t t = 0;
    for (; t < 500; t += 5) {
        const double k = t / 500.0;
        Truth now = steady(end.lean_deg * k, end.mph, end.steer_deg != 0.0);
        now.rake_deg = end.rake_deg;
        now.mount_roll_deg = end.mount_roll_deg;
        now.mount_pitch_deg = end.mount_pitch_deg;
        now.roll_dps = end.lean_deg / 0.5;
        f.update(sense(now, t), cfg);
    }
    for (; t < 500 + hold_ms; t += 5) {
        f.update(sense(end, t), cfg);
        each();
    }
}

// A moment of a ride: the truth the IMU sees, and how fast the *path* turns,
// which is what the GPS course follows. They differ only in a slide.
struct Moment { Truth truth; double path_dps; };

// 5 ms IMU samples and a 10 Hz GPS. The course starts at 350° (so a right
// turn wraps through north), carries ±`noise_deg`, and is stamped `lag_ms`
// after it was true: the receiver's quickest latency, which core::fixtime
// cannot take out.
template <class At, class Each>
void ride_gps(Filter& f, const Config& cfg, uint32_t ms, bool gps, At&& at, Each&& each,
              uint32_t lag_ms = 40, double noise_deg = 0.5) {
    std::vector<double> path(ms / 5 + 1, 350.0);
    uint32_t seed = 1;
    for (uint32_t t = 0, i = 0; t < ms; t += 5, ++i) {
        const Moment m = at(t);
        if (i > 0) path[i] = path[i - 1] + m.path_dps * 0.005;
        f.update(sense(m.truth, t), cfg);
        if (gps && t % 100 == 0 && t >= lag_ms) {
            seed = seed * 1103515245u + 12345u;
            const double noise = noise_deg * (((seed >> 16) & 0x7fff) / 16383.5 - 1.0);
            const double course = std::fmod(path[(t - lag_ms) / 5] + noise + 3600.0, 360.0);
            f.on_fix(static_cast<float>(course), static_cast<float>(m.truth.mph), t, true, cfg);
        }
        each(t);
    }
}

Config frame_mounted() {
    Config c;
    c.wheelbase_ft = 0.0f;
    return c;
}
}  // namespace

int main() {
    const Config cfg;

    CASE("upright and stationary reads zero");
    {
        Filter f;
        Sample s;
        s.az_g = 1.0f;
        for (uint32_t t = 0; t < 2000; t += 10) { s.t_ms = t; f.update(s, cfg); }
        CHECK_NEAR(f.lean_deg(), 0.0, 0.1);
        CHECK_NEAR(f.pitch_deg(), 0.0, 0.1);
    }

    CASE("parked on the side stand reads the real angle");
    {
        // Not moving, so the accelerometer is the honest reference here.
        Filter f;
        Sample s;
        const float lean = 25.0f;
        s.ay_g = std::sin(lean * kDeg2Rad);
        s.az_g = std::cos(lean * kDeg2Rad);
        for (uint32_t t = 0; t < 3000; t += 10) { s.t_ms = t; f.update(s, cfg); }
        CHECK_NEAR(f.lean_deg(), 25.0, 0.5);
    }

    CASE("steady turns read their lean, where an accelerometer fails");
    {
        // The case that defeats an accelerometer: the resultant points
        // straight down through the bike. It also caught the filter using
        // the yaw gyro as the world's yaw rate — that read 33° here — and
        // integrating the pitch gyro, which reads Ω·sin φ in a corner, as
        // pitch: past 30° of lean it climbed to the clamp.
        const Config cfg = frame_mounted();
        for (double lean : {10.0, 20.0, 30.0, 40.0, 50.0, -25.0, -45.0}) {
            Filter f;
            float worst_pitch = 0.0f;
            ride_into(f, cfg, steady(lean, 35.0, false), 6000, [&] {
                worst_pitch = std::max(worst_pitch, std::fabs(f.pitch_deg()));
            });
            CHECK_NEAR(f.lean_deg(), lean, 1.0);
            CHECK(worst_pitch < 1.5f);
            CHECK(std::fabs(f.accel_lean_deg()) < 1.0f);
        }
    }

    CASE("the yaw rate published is the world's, > 0 turning right");
    {
        const Config cfg = frame_mounted();
        const Truth t = steady(40.0, 35.0, false);
        Filter f;
        ride_into(f, cfg, t, 3000, [] {});
        core::state::Attitude a;
        f.publish_into(a);
        CHECK_NEAR(a.yaw_rate_dps, t.yaw_dps, 0.5);
        CHECK(a.yaw_rate_dps > 0.0f);
    }

    CASE("a right-handed part reads a right turn as left yaw, and that is right");
    {
        // The frame is x forward, y left, z up, as a real part reads it. A
        // right turn is gz < 0 there; get it backwards and the reference
        // pulls a right lean to the left.
        const Truth t = steady(30.0, 35.0, false);
        const Sample s = sense(t, 0);
        CHECK(s.gz_dps < 0.0f);
        CHECK(s.gy_dps < 0.0f);          // Ω·sin φ, nose up is gy < 0
        CHECK(s.ay_g == 0.0f || std::fabs(s.ay_g) < 1e-6f);
    }

    CASE("steady turns read their lean with the IMU on the turned bars");
    {
        // The bars turn about the raked head tube, so they tip part of the
        // yaw rate onto the roll gyro: 1.5 dps at 40° and 30 mph, 9 dps at
        // 20° and 8 mph, and a complementary filter turns a steady rate into
        // an angle of rate × τ. The estimate from the kinematics takes it out.
        const Config bars;                       // the default: on the bars
        const Config blind = frame_mounted();    // the same, uncorrected
        struct Corner { double lean, mph; };
        for (Corner k : {Corner{40.0, 30.0}, Corner{30.0, 15.0}, Corner{20.0, 8.0},
                         Corner{-30.0, 15.0}}) {
            const Truth t = steady(k.lean, k.mph, true);
            Filter f, g;
            ride_into(f, bars, t, 6000, [] {});
            ride_into(g, blind, t, 6000, [] {});
            CHECK_NEAR(f.lean_deg(), k.lean, 1.0);
            CHECK_NEAR(f.steer_deg(), t.steer_deg, 0.5);
            CHECK(std::fabs(f.pitch_deg()) < 1.5f);
            // Not a test of the filter, a test that this one is a real
            // case: uncorrected, the slow corners were well off.
            if (std::fabs(k.mph) < 20.0) CHECK(std::fabs(g.lean_deg() - k.lean) > 2.5f);
        }
    }

    CASE("a caught slide does not throw the lean");
    {
        // A 30° right-hander at 25 mph on the bars. At 3 s the rear steps
        // out: the heading runs ahead of the path for 0.3 s and is caught in
        // the next 0.3 s, with opposite lock on the bars while it lasts. The
        // bike's lean does not change. Without the GPS check v·Ω and the
        // steering estimate both read the slide as lean: ~7° off.
        const Truth corner = steady(30.0, 25.0, true);
        auto at = [&](uint32_t t) {
            const double e = (t - 3000.0) / 1000.0;
            Truth now = steady(30.0 * std::min(1.0, t / 500.0), 25.0, true);
            now.roll_dps = t < 500 ? 60.0 : 0.0;
            const double path = now.yaw_dps;
            if (e >= 0.0 && e < 0.6) {
                now.yaw_dps += e < 0.3 ? 70.0 : -70.0;
                now.steer_deg = corner.steer_deg - 10.0 * (e < 0.3 ? e / 0.3 : (0.6 - e) / 0.3);
            }
            return Moment{now, path};
        };
        const Config cfg;
        Filter f, blind;
        float worst = 0.0f, worst_blind = 0.0f;
        bool slid = false;
        ride_gps(f, cfg, 5400, true, at, [&](uint32_t t) {
            if (t > 1500) worst = std::max(worst, std::fabs(f.lean_deg() - 30.0f));
            slid = slid || f.sliding();
        });
        ride_gps(blind, cfg, 5400, false, at, [&](uint32_t t) {
            if (t > 1500) worst_blind = std::max(worst_blind, std::fabs(blind.lean_deg() - 30.0f));
        });
        CHECK(slid);
        CHECK(!f.sliding());                   // and it ended
        CHECK(worst < 3.0f);
        CHECK_NEAR(f.lean_deg(), 30.0, 1.0);
        CHECK(worst_blind > 5.0f);             // the case is a real one
    }

    CASE("corners, GPS noise and latency and north do not look like a slide");
    {
        // Rolling in and out of corners both ways with the course wrapping
        // through 0/360, ±0.5° of course noise and 40 ms of fix latency.
        const Config cfg;
        for (double lean : {40.0, -35.0}) {
            Filter f;
            bool slid = false;
            float worst_rate = 0.0f;
            ride_gps(f, cfg, 8000, true, [&](uint32_t t) {
                // Straight, roll in over 0.5 s at 1 s, hold, roll out at 5 s.
                double k = t < 1000 ? 0.0 : t < 1500 ? (t - 1000) / 500.0
                         : t < 5000 ? 1.0 : t < 5500 ? (5500 - t) / 500.0 : 0.0;
                Truth now = steady(lean * k, 30.0, true);
                now.roll_dps = (t >= 1000 && t < 1500) ? lean / 0.5
                             : (t >= 5000 && t < 5500) ? -lean / 0.5 : 0.0;
                return Moment{now, now.yaw_dps};
            }, [&](uint32_t) {
                slid = slid || f.sliding();
                worst_rate = std::max(worst_rate, std::fabs(f.slide_rate_dps()));
            });
            CHECK(!slid);
            CHECK(worst_rate < 0.5f * cfg.slide_rate_dps);
            CHECK(std::fabs(f.lean_deg()) < 1.0f);
        }
    }

    CASE("a fix without a course starts the slide window over");
    {
        Filter f;
        const Config cfg;
        Truth straight;
        straight.mph = 30.0;
        uint32_t t = 0;
        for (; t <= 100; t += 5) f.update(sense(straight, t), cfg);
        f.on_fix(10.0f, 30.0f, 100, true, cfg);
        for (; t <= 200; t += 5) f.update(sense(straight, t), cfg);
        f.on_fix(0.0f, 30.0f, 200, false, cfg);   // no course
        for (; t <= 300; t += 5) f.update(sense(straight, t), cfg);
        f.on_fix(90.0f, 30.0f, 300, true, cfg);   // would be 400 dps
        for (; t <= 400; t += 5) f.update(sense(straight, t), cfg);
        f.on_fix(90.0f, 30.0f, 400, true, cfg);
        CHECK(!f.sliding());
        // And a fix stamped after the last sample is not compared.
        f.on_fix(90.0f, 30.0f, 500, true, cfg);
        CHECK(!f.sliding());
    }

    CASE("a wheelie is pitch up, and the pitch gyro says so with gy < 0");
    {
        // Pop the front at 50 dps for 0.4 s under hard drive, which closes
        // the accelerometer gate, so this is the gyro alone.
        Filter f;
        const Config cfg;
        uint32_t t = 0;
        Truth flat;
        flat.mph = 30.0;
        for (; t < 1000; t += 5) f.update(sense(flat, t), cfg);
        Truth pop = flat;
        pop.f = {0.8, 0, 1.0};
        pop.pitch_dps = 50.0;
        for (; t < 1400; t += 5) {
            pop.pitch_deg = 50.0 * (t - 1000) / 1000.0;
            f.update(sense(pop, t), cfg);
        }
        CHECK_NEAR(f.pitch_deg(), 20.0, 1.0);
        CHECK_NEAR(f.lean_deg(), 0.0, 0.5);
    }

    CASE("gyro drift stays bounded — this is what the correction is for");
    {
        // A part with a 2 deg/s bias, uncorrected, would read 600 degrees
        // after five minutes. The reference term has to pin it.
        Filter f;
        Truth straight;
        straight.mph = 40.0;
        Sample base = sense(straight, 0);
        for (uint32_t t = 0; t < 300000; t += 10) {
            Sample s = base;
            s.gx_dps = 2.0f;                        // pure bias, no real roll
            s.gz_dps = 0.0f;
            s.t_ms = t;
            f.update(s, cfg);
        }
        CHECK(std::fabs(f.lean_deg()) < 5.0f);
    }

    CASE("a known gyro bias is subtracted outright");
    {
        Filter f;
        f.set_gyro_bias(2.0f, 0.0f, 0.0f);
        Sample s;
        s.gx_dps = 2.0f;
        s.az_g = 1.0f;
        for (uint32_t t = 0; t < 60000; t += 10) { s.t_ms = t; f.update(s, cfg); }
        CHECK_NEAR(f.lean_deg(), 0.0, 0.5);
    }

    CASE("the mount offset comes out of every reading");
    {
        // Board bolted in 8 degrees off level.
        Filter f;
        Sample s;
        const float mount = 8.0f;
        s.ay_g = std::sin(mount * kDeg2Rad);
        s.az_g = std::cos(mount * kDeg2Rad);

        float roll = 0.0f, pitch = 0.0f;
        f.calibrate_level(s, roll, pitch);
        CHECK_NEAR(roll, 8.0, 0.1);

        for (uint32_t t = 0; t < 2000; t += 10) { s.t_ms = t; f.update(s, cfg); }
        CHECK_NEAR(f.lean_deg(), 0.0, 0.2);        // upright, despite the mount
    }

    CASE("a dash tilted back towards the rider is turned out, not subtracted");
    {
        // 30° of mount pitch. Subtracting angles left the side stand at 28°
        // for 25°, and folded the yaw rate into roll in every corner.
        const Config cfg = frame_mounted();
        Truth level;
        level.mount_pitch_deg = 30.0;
        level.mount_roll_deg = 4.0;
        float roll = 0.0f, pitch = 0.0f;
        Filter f;
        f.calibrate_level(sense(level, 0), roll, pitch);
        CHECK_NEAR(roll, 4.0, 0.1);
        CHECK_NEAR(pitch, 30.0, 0.1);

        Truth stand = level;
        stand.lean_deg = -25.0;
        stand.f = {0, -std::sin(25.0 * kD), std::cos(25.0 * kD)};
        for (uint32_t t = 0; t < 3000; t += 5) f.update(sense(stand, t), cfg);
        CHECK_NEAR(f.lean_deg(), -25.0, 0.5);

        Filter g;
        g.set_mount_offset(roll, pitch);
        Truth corner = steady(40.0, 35.0, false);
        corner.mount_pitch_deg = 30.0;
        corner.mount_roll_deg = 4.0;
        ride_into(g, cfg, corner, 6000, [] {});
        CHECK_NEAR(g.lean_deg(), 40.0, 1.0);
        CHECK(std::fabs(g.pitch_deg()) < 1.5f);
    }

    CASE("hard braking is not read as the bike pitching");
    {
        // 0.8 g of deceleration tips the specific-force vector back by 38
        // degrees. Gating the correction on |a| ~= 1 g is what stops the
        // pitch gauge from swinging every time the rider grabs the brake.
        Filter f;
        Sample s;
        s.az_g = 1.0f;
        for (uint32_t t = 0; t < 1000; t += 10) { s.t_ms = t; f.update(s, cfg); }
        CHECK_NEAR(f.pitch_deg(), 0.0, 0.2);

        for (uint32_t t = 1000; t < 2000; t += 10) {
            s.ax_g = -0.8f;                        // braking
            s.az_g = 1.0f;
            s.gy_dps = 0.0f;                       // chassis is not rotating
            s.t_ms = t;
            f.update(s, cfg);
        }
        CHECK(std::fabs(f.pitch_deg()) < 3.0f);
    }

    CASE("a dropped sample block does not integrate a gap");
    {
        Filter f;
        Sample s;
        s.az_g = 1.0f;
        s.t_ms = 0;
        f.update(s, cfg);
        s.t_ms = 100;
        f.update(s, cfg);
        const float before = f.lean_deg();
        s.gx_dps = 50.0f;
        s.t_ms = 5000;                              // a five-second hole
        f.update(s, cfg);
        CHECK_NEAR(f.lean_deg(), before, 1e-4);     // no rotation invented
    }

    CASE("the published g vector does not alias engine vibration");
    {
        // 0.3 g forward, plus 2 g of 75 Hz vibration on the same axis. Read
        // at 50 Hz like logic does, the raw sample swung by up to 2 g.
        Filter f;
        Sample s;
        float worst = 0.0f;
        for (int i = 0; i < 400; ++i) {
            s.t_ms = static_cast<uint32_t>(i * 5);
            s.ax_g = 0.3f + 2.0f * std::sin(2.0f * 3.14159265f * 75.0f * s.t_ms / 1000.0f);
            f.update(s, cfg);
            if (i > 100 && i % 4 == 0) {
                core::state::Attitude a;
                f.publish_into(a);
                worst = std::max(worst, std::fabs(a.g_lon - 0.3f));
            }
        }
        CHECK(worst < 0.5f);
    }

    CASE("the published attitude carries the g vector as well");
    {
        Filter f;
        Sample s;
        s.ax_g = 0.1f; s.ay_g = -0.2f; s.az_g = 1.0f;
        s.gz_dps = 12.0f;                          // yawing left
        s.t_ms = 0;
        f.update(s, cfg);
        core::state::Attitude a;
        f.publish_into(a);
        CHECK_NEAR(a.g_lon, 0.1, 1e-6);
        CHECK_NEAR(a.g_lat, -0.2, 1e-6);
        CHECK_NEAR(a.g_vert, 1.0, 1e-6);
        // The yaw rate needs a second sample, and is the world's, right +.
        s.t_ms = 5;
        f.update(s, cfg);
        f.publish_into(a);
        CHECK(a.yaw_rate_dps < -11.0f && a.yaw_rate_dps > -13.0f);
    }

    CASE("a FIFO batch is back-dated so no two samples share a timestamp");
    {
        // The newest sample is the one just taken; the rest step back by the
        // sample period, oldest first.
        const uint32_t now = 100000;
        CHECK(sample_time_ms(now, 3, 4, 5) == now);          // newest
        CHECK(sample_time_ms(now, 2, 4, 5) == now - 5);
        CHECK(sample_time_ms(now, 1, 4, 5) == now - 10);
        CHECK(sample_time_ms(now, 0, 4, 5) == now - 15);     // oldest

        // Strictly increasing across a full batch, which is the property the
        // filter needs: equal neighbours are dropped, and a decreasing pair
        // reads as an enormous unsigned gap and is dropped too.
        for (size_t n = 1; n <= 16; ++n) {
            for (size_t i = 1; i < n; ++i) {
                CHECK(sample_time_ms(now, i, n, 5) >
                      sample_time_ms(now, i - 1, n, 5));
            }
        }

        // A single sample is simply now.
        CHECK(sample_time_ms(now, 0, 1, 5) == now);
        // Unsigned arithmetic across the 49-day wrap stays consistent: the
        // differences are what the filter reads, and they still come out at
        // one period apart.
        const uint32_t near_wrap = 3;
        CHECK(static_cast<uint32_t>(sample_time_ms(near_wrap, 1, 2, 5) -
                                    sample_time_ms(near_wrap, 0, 2, 5)) == 5);
    }

    CASE("every sample in a FIFO batch is integrated, not just the first");
    {
        // Why per-sample timestamps matter, and it is not latency.
        //
        // A driver that stamps a whole FIFO batch with the time it was read
        // hands this filter one usable sample and the rest with dt == 0,
        // which the guard above drops. The 200 Hz stream silently becomes
        // the poll rate — and on a dirt bike what lives between those two
        // rates is vibration. Alias it and you integrate whichever phase you
        // happened to land on.
        //
        // Here the roll gyro shakes at +/-60 dps every sample and nets to
        // zero over each batch of four. Reading all four keeps the bike
        // upright. Keeping only the first integrates +60 dps forever.
        Config cfg;
        auto shake = [](uint32_t t_ms, int k) {
            Sample s;
            s.gx_dps = (k % 2 == 0) ? 60.0f : -60.0f;
            s.gy_dps = 0.0f;
            s.gz_dps = 0.0f;
            s.ax_g = 0.0f;
            s.ay_g = 0.0f;
            s.az_g = 1.0f;              // upright
            s.speed_mph = 25.0f;
            s.t_ms = t_ms;
            return s;
        };

        Filter all, first_only;
        int k = 0;
        for (uint32_t base = 0; base < 3000; base += 20) {
            for (int i = 0; i < 4; ++i, ++k) {
                all.update(shake(base + i * 5, k), cfg);
            }
            // What the batched-timestamp driver effectively produced: one
            // sample per read, carrying the whole interval.
            first_only.update(shake(base + 15, k - 4), cfg);
        }

        core::state::Attitude a{}, b{};
        all.publish_into(a);
        first_only.publish_into(b);

        // Reading the whole batch, the shake cancels and the bike is upright.
        CHECK_NEAR(a.lean_deg, 0.0, 2.0);
        // Reading one in four, it does not. This is the failure the driver
        // used to produce, asserted so nobody reintroduces it.
        CHECK(std::fabs(b.lean_deg) > 5.0f);
    }

    CASE("Calibrate level averages a still second, and refuses a bike that moved");
    {
        // A board pitched 30° on the bars, rolled 4°, with a 1.5 dps bias on
        // its roll gyro and a little noise on everything.
        Truth level;
        level.mount_pitch_deg = 30.0;
        level.mount_roll_deg = 4.0;
        LevelCal cal;
        CHECK(cal.feed(sense(level, 0)) == LevelCal::Result::Idle);
        cal.start();
        LevelCal::Result r = LevelCal::Result::Pending;
        uint32_t seed = 7;
        for (uint32_t i = 0; i < LevelCal::kSamples && r == LevelCal::Result::Pending; ++i) {
            Sample s = sense(level, i * 5);
            seed = seed * 1103515245u + 12345u;
            const float n = (((seed >> 16) & 0xff) / 127.5f - 1.0f);
            s.ax_g += 0.01f * n;
            s.az_g -= 0.01f * n;
            s.gx_dps = 1.5f + 0.3f * n;
            r = cal.feed(s);
        }
        CHECK(r == LevelCal::Result::Done);
        CHECK(!cal.active());
        CHECK_NEAR(cal.mean().gx_dps, 1.5, 0.05);

        // Into the filter: the offsets it stores, and level reads level.
        Filter f;
        float roll = 0.0f, pitch = 0.0f;
        f.calibrate_level(cal.mean(), roll, pitch);
        f.set_gyro_bias(cal.mean().gx_dps, cal.mean().gy_dps, cal.mean().gz_dps);
        CHECK_NEAR(roll, 4.0, 0.2);
        CHECK_NEAR(pitch, 30.0, 0.2);
        const Config cfg = frame_mounted();
        for (uint32_t t = 0; t < 3000; t += 5) {
            Sample s = sense(level, t);
            s.gx_dps = 1.5f;
            f.update(s, cfg);
        }
        CHECK_NEAR(f.lean_deg(), 0.0, 0.3);

        // A shove part way through: nothing is stored.
        cal.start();
        for (uint32_t i = 0; i < 50; ++i) CHECK(cal.feed(sense(level, i * 5)) == LevelCal::Result::Pending);
        Truth shoved = level;
        shoved.roll_dps = 20.0;
        CHECK(cal.feed(sense(shoved, 250)) == LevelCal::Result::Moved);
        CHECK(!cal.active());
    }

    CASE("a slow turn under the gyro limit is not still");
    {
        // The bars eased round at 4 dps: every sample is under kStillDps and
        // within kStillG of 1 g, so only the gravity vector moving gives it
        // away. Averaged, it would be a 4 dps bias. Here about the board's
        // x axis, so gravity swings across y; refused once it has turned 2°.
        auto turning = [](float dps, uint32_t i) {
            Sample s;
            const float ang = dps * static_cast<float>(i) * 0.005f * 3.14159265f / 180.0f;
            s.gx_dps = dps;
            s.ay_g = std::sin(ang);
            s.az_g = std::cos(ang);
            s.t_ms = i * 5;
            return s;
        };
        LevelCal cal;
        cal.start();
        LevelCal::Result r = LevelCal::Result::Pending;
        uint32_t i = 0;
        for (; i < LevelCal::kSamples && r == LevelCal::Result::Pending; ++i) r = cal.feed(turning(4.0f, i));
        CHECK(r == LevelCal::Result::Moved);
        CHECK(i < LevelCal::kSamples);
        // At 1.5 dps it turns 1.5° in the second and is let through: the
        // error it can leave is bounded, not zero.
        cal.start();
        r = LevelCal::Result::Pending;
        for (i = 0; i < LevelCal::kSamples && r == LevelCal::Result::Pending; ++i) r = cal.feed(turning(1.5f, i));
        CHECK(r == LevelCal::Result::Done);
    }

    return check::report("attitude");
}
