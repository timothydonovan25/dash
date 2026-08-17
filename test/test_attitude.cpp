#include "check.hpp"
#include "core/attitude/attitude.hpp"
#include <cmath>

using namespace core::attitude;

namespace {
constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
constexpr float kG = 32.174f;
constexpr float kFtPerSPerMph = 1.46666667f;

// A bike in a steady, coordinated turn. This is the case that defeats an
// accelerometer: the machine leans into the resultant, so the accelerometer
// reads straight "down" through the bike and sees almost no lean at all,
// while the gyro sees a yaw rate and the rider sees the horizon at 40°.
struct SteadyTurn {
    float lean_deg;
    float speed_mph;

    // The yaw rate that holds this lean at this speed: tan(lean) = v·ω/g.
    float yaw_rate_dps() const {
        const float v = speed_mph * kFtPerSPerMph;
        const float w = std::tan(lean_deg * kDeg2Rad) * kG / v;   // rad/s
        return w / kDeg2Rad;
    }

    // What the accelerometer reads: the resultant, which is longer than 1 g
    // and points down through the bike's own vertical.
    float mag_g() const { return 1.0f / std::cos(lean_deg * kDeg2Rad); }
};

Sample turn_sample(const SteadyTurn& t, uint32_t t_ms) {
    Sample s;
    s.gx_dps = 0.0f;                       // steady: not rolling any further
    s.gy_dps = 0.0f;
    s.gz_dps = t.yaw_rate_dps();
    s.ax_g   = 0.0f;
    s.ay_g   = 0.0f;                       // the resultant is straight down
    s.az_g   = t.mag_g();                  //   through the bike
    s.speed_mph = t.speed_mph;
    s.t_ms   = t_ms;
    return s;
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

    CASE("a steady 40 degree turn is measured, where an accelerometer fails");
    {
        const SteadyTurn turn{40.0f, 45.0f};
        Filter f;
        // Roll in over half a second, then hold.
        uint32_t t = 0;
        for (; t < 500; t += 10) {
            Sample s = turn_sample(turn, t);
            s.gx_dps = 40.0f / 0.5f;               // rolling right
            s.gz_dps = turn.yaw_rate_dps() * (static_cast<float>(t) / 500.0f);
            s.ay_g = 0.0f;
            s.az_g = 1.0f;
            f.update(s, cfg);
        }
        for (; t < 6000; t += 10) f.update(turn_sample(turn, t), cfg);

        CHECK_NEAR(f.lean_deg(), 40.0, 3.0);
        // The whole point: what the accelerometer alone would have said.
        CHECK_NEAR(f.accel_lean_deg(), 0.0, 1.0);
        CHECK(std::fabs(f.accel_lean_deg()) < 5.0f);
    }

    CASE("a left turn is negative");
    {
        const SteadyTurn turn{-35.0f, 50.0f};
        Filter f;
        uint32_t t = 0;
        for (; t < 500; t += 10) {
            Sample s = turn_sample(turn, t);
            s.gx_dps = -35.0f / 0.5f;
            s.az_g = 1.0f;
            f.update(s, cfg);
        }
        for (; t < 6000; t += 10) f.update(turn_sample(turn, t), cfg);
        CHECK_NEAR(f.lean_deg(), -35.0, 3.0);
    }

    CASE("gyro drift stays bounded — this is what the correction is for");
    {
        // A part with a 2 deg/s bias, uncorrected, would read 600 degrees
        // after five minutes. The reference term has to pin it.
        Filter f;
        const SteadyTurn straight{0.0f, 40.0f};
        Sample base = turn_sample(straight, 0);
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

    CASE("the published attitude carries the g vector as well");
    {
        Filter f;
        Sample s;
        s.ax_g = 0.1f; s.ay_g = -0.2f; s.az_g = 1.0f;
        s.gz_dps = 12.0f;
        s.t_ms = 0;
        f.update(s, cfg);
        core::state::Attitude a;
        f.publish_into(a);
        CHECK_NEAR(a.g_lon, 0.1, 1e-6);
        CHECK_NEAR(a.g_lat, -0.2, 1e-6);
        CHECK_NEAR(a.g_vert, 1.0, 1e-6);
        CHECK_NEAR(a.yaw_rate_dps, 12.0, 1e-6);
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

    return check::report("attitude");
}
