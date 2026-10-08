#include "check.hpp"
#include "core/sweep/sweep.hpp"

using namespace core::sweep;
using core::state::Frame;

int main() {
    CASE("the level rises to full scale halfway and falls back to zero");
    {
        CHECK_NEAR(level(0, 2000), 0.0, 1e-6);
        CHECK_NEAR(level(500, 2000), 0.5, 1e-6);
        CHECK_NEAR(level(1000, 2000), 1.0, 1e-6);
        CHECK_NEAR(level(1500, 2000), 0.5, 1e-6);
        CHECK(level(1999, 2000) < 0.01f);
        CHECK(level(2000, 2000) < 0.0f);
        CHECK(level(0, 0) < 0.0f);
    }

    CASE("it climbs monotonically, then falls monotonically");
    {
        float prev = -1.0f;
        for (uint32_t t = 0; t <= 1000; t += 10) {
            CHECK(level(t, 2000) >= prev);
            prev = level(t, 2000);
        }
        for (uint32_t t = 1000; t < 2000; t += 10) {
            CHECK(level(t, 2000) <= prev);
            prev = level(t, 2000);
        }
    }

    CASE("at the top: peak rpm, peak speed, top gear, not neutral");
    {
        Frame f;
        f.engine.neutral = true;
        Config c;
        c.peak_rpm = 9000.0f;
        c.gears = 5;
        CHECK(apply(f, 1000, c));
        CHECK_NEAR(f.engine.rpm, 9000.0, 1e-3);
        CHECK_NEAR(f.motion.best_mph, kPeakMph, 1e-3);
        CHECK_NEAR(f.motion.front_mph, kPeakMph, 1e-3);
        CHECK(f.engine.gear == 5);
        CHECK(!f.engine.neutral);
    }

    CASE("at the bottom it reads first gear and zero");
    {
        Frame f;
        Config c;
        CHECK(apply(f, 0, c));
        CHECK(f.engine.gear == 1);
        CHECK_NEAR(f.engine.rpm, 0.0, 1e-3);
    }

    CASE("finished, disabled, or moving: the frame is left alone");
    {
        Config c;
        Frame f;
        f.engine.rpm = 1500.0f;
        CHECK(!apply(f, 2000, c));
        CHECK_NEAR(f.engine.rpm, 1500.0, 1e-6);

        c.enabled = false;
        CHECK(!apply(f, 1000, c));
        CHECK_NEAR(f.engine.rpm, 1500.0, 1e-6);

        c.enabled = true;
        f.motion.best_mph = 12.0f;
        CHECK(!apply(f, 1000, c));
        CHECK_NEAR(f.motion.best_mph, 12.0, 1e-6);
    }

    CASE("the duration is the setting's, not a constant");
    {
        Config c;
        c.duration_ms = 4000;
        Frame f;
        CHECK(apply(f, 3000, c));
        CHECK(!apply(f, 4000, c));
    }

    CASE("the lamps sweep the same rpm the tach readout does");
    {
        Config c;
        c.peak_rpm = 8800.0f;
        for (uint32_t t = 0; t < c.duration_ms; t += 50) {
            Frame f;
            CHECK(apply(f, t, c));
            CHECK_NEAR(lamp_rpm(t, 0.0f, c), f.engine.rpm, 1e-3);
        }
        CHECK(lamp_rpm(c.duration_ms, 0.0f, c) < 0.0f);
        CHECK(lamp_rpm(1000, 5.0f, c) < 0.0f);          // moving
        c.enabled = false;
        CHECK(lamp_rpm(1000, 0.0f, c) < 0.0f);
    }

    return check::report("sweep");
}
