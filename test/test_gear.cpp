#include "check.hpp"
#include "core/gear/gear.hpp"

using namespace core::gear;

namespace {
// Speed that puts the bike squarely in gear `g` at the given rpm.
float speed_for(const Config& c, uint8_t g, float rpm) {
    return rpm / c.rpm_per_mph[g - 1];
}
}  // namespace

int main() {
    const Config cfg;

    CASE("each gear is identified from its own ratio");
    {
        Estimator e;
        uint32_t t = 0;
        for (uint8_t g = 1; g <= cfg.count; ++g) {
            const float rpm = 6000.0f;
            const float mph = speed_for(cfg, g, rpm);
            t += 100;
            CHECK(e.update(rpm, mph, false, t, cfg) == g);
        }
    }

    CASE("the same gear holds across its whole rev range");
    {
        Estimator e;
        uint32_t t = 0;
        for (float rpm = 2000.0f; rpm <= 9000.0f; rpm += 250.0f) {
            t += 100;
            const float mph = speed_for(cfg, 3, rpm);
            CHECK(e.update(rpm, mph, false, t, cfg) == 3);
        }
    }

    CASE("neutral is sensed, not inferred, and wins outright");
    {
        Estimator e;
        uint32_t t = 100;
        e.update(6000.0f, speed_for(cfg, 2, 6000.0f), false, t, cfg);
        CHECK(e.gear() == 2);
        t += 100;
        // Rolling to a stop in neutral: the ratio still looks like second.
        CHECK(e.update(6000.0f, speed_for(cfg, 2, 6000.0f), true, t, cfg)
              == kUnknown);
    }

    CASE("a clutch pull holds the gear, then gives up");
    {
        // The moment a rider looks at a gear indicator is the moment it is
        // least able to know, so it holds rather than blinking out and back.
        Estimator e;
        uint32_t t = 0;
        t += 100;
        e.update(6000.0f, speed_for(cfg, 3, 6000.0f), false, t, cfg);
        CHECK(e.gear() == 3);

        // Clutch in, revs flare, road speed unchanged.
        const float mph = speed_for(cfg, 3, 6000.0f);
        for (int i = 0; i < 4; ++i) {
            t += 100;
            CHECK(e.update(9500.0f, mph, false, t, cfg) == 3);   // held
        }
        t += 400;
        CHECK(e.update(9500.0f, mph, false, t, cfg) == kUnknown);
    }

    CASE("a real shift lands in the new gear rather than blanking");
    {
        Estimator e;
        uint32_t t = 0;
        t += 100;
        e.update(8000.0f, speed_for(cfg, 2, 8000.0f), false, t, cfg);
        CHECK(e.gear() == 2);

        // Clutch in for 200 ms at the same road speed, then out in third.
        const float mph = speed_for(cfg, 2, 8000.0f);
        t += 100;  e.update(9800.0f, mph, false, t, cfg);
        t += 100;  e.update(7000.0f, mph, false, t, cfg);
        t += 100;
        CHECK(e.update(mph * cfg.rpm_per_mph[2], mph, false, t, cfg) == 3);
    }

    CASE("stopped is not a gear");
    {
        Estimator e;
        uint32_t t = 0;
        t += 100;
        e.update(6000.0f, speed_for(cfg, 1, 6000.0f), false, t, cfg);
        CHECK(e.gear() == 1);
        // Dividing by a speed near zero gives a ratio near infinity, and one
        // that swings wildly for a tenth of a mph of GPS noise.
        for (int i = 0; i < 12; ++i) {
            t += 100;
            e.update(1500.0f, 0.3f, false, t, cfg);
        }
        CHECK(e.gear() == kUnknown);
    }

    CASE("idling in gear with the clutch in is not a gear either");
    {
        Estimator e;
        uint32_t t = 0;
        for (int i = 0; i < 12; ++i) {
            t += 100;
            e.update(1400.0f, 0.0f, false, t, cfg);
        }
        CHECK(e.gear() == kUnknown);
    }

    CASE("a ratio off the end of the table matches nothing");
    {
        // Wheelspin in first: the engine is turning far faster than the road
        // speed accounts for, so the ratio is above anything the gearbox can
        // produce. Same shape of reading as a badly slipping clutch.
        Estimator e;
        uint32_t t = 0;
        for (int i = 0; i < 12; ++i) {
            t += 100;
            e.update(6000.0f, 6000.0f / 620.0f, false, t, cfg);
        }
        CHECK(e.gear() == kUnknown);

        // And the other end: coasting at speed with the engine near idle.
        Estimator f;
        t = 0;
        for (int i = 0; i < 12; ++i) {
            t += 100;
            f.update(2500.0f, 2500.0f / 95.0f, false, t, cfg);
        }
        CHECK(f.gear() == kUnknown);
    }

    CASE("the default ratios are far enough apart for the tolerance");
    {
        // If two gears' tolerance bands overlapped, a single ratio would sit
        // inside both and the reading would depend on table order rather than
        // on the bike. This is the guard on anyone recalibrating the table.
        for (uint8_t i = 0; i + 1 < cfg.count; ++i) {
            const float hi = cfg.rpm_per_mph[i];
            const float lo = cfg.rpm_per_mph[i + 1];
            const float hi_low_edge = hi * (1.0f - cfg.tolerance_pct / 100.0f);
            const float lo_high_edge = lo * (1.0f + cfg.tolerance_pct / 100.0f);
            if (hi_low_edge <= lo_high_edge) {
                std::printf("       gears %d and %d overlap: %.0f vs %.0f\n",
                            i + 1, i + 2, hi_low_edge, lo_high_edge);
            }
            CHECK(hi_low_edge > lo_high_edge);
        }
    }

    CASE("no flicker between neighbours at a steady cruise");
    {
        // 1% of ratio noise must not move the reading, or the tile becomes
        // unreadable at exactly the speeds the bike spends its life at.
        Estimator e;
        uint32_t t = 0;
        int changes = 0;
        uint8_t last = kUnknown;
        for (int i = 0; i < 400; ++i) {
            t += 50;
            const float wobble = 1.0f + ((i % 7) - 3) * 0.003f;
            const float mph = speed_for(cfg, 4, 6500.0f) * wobble;
            const uint8_t g = e.update(6500.0f, mph, false, t, cfg);
            if (g != last) { ++changes; last = g; }
        }
        CHECK(changes == 1);          // only the very first reading
        CHECK(last == 4);
    }

    CASE("the measured ratio is exposed so the table can be calibrated");
    {
        Estimator e;
        e.update(6000.0f, 30.0f, false, 100, cfg);
        CHECK_NEAR(e.last_ratio(), 200.0, 0.01);
    }

    CASE("a table with fewer gears than the maximum is respected");
    {
        Config four = cfg;
        four.count = 4;
        Estimator e;
        // Fifth's ratio, on a four-speed table, is 21% off fourth: no match.
        uint32_t t = 0;
        for (int i = 0; i < 12; ++i) {
            t += 100;
            e.update(6000.0f, speed_for(cfg, 5, 6000.0f), false, t, four);
        }
        CHECK(e.gear() == kUnknown);
    }

    return check::report("gear");
}
