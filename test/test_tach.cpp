#include "check.hpp"
#include "core/tach/tach.hpp"

using namespace core::tach;

namespace {
// The ctrl task at 500 Hz: 2 ms per tick. Pulses land whenever the engine
// puts them out, and the driver's ISR timestamps each one — which is what
// makes the reading independent of the polling rate.
struct Ctrl {
    Meter    m;
    Config   c;
    uint32_t total = 0;
    uint64_t t_us = 0;
    uint64_t edge_us = 0;
    double   next_pulse_us = -1.0;

    void run(float rpm, uint32_t ms) {
        const double period = rpm > 0.0f
                                  ? 60000000.0 / (rpm * c.pulses_per_rev)
                                  : 0.0;
        next_pulse_us = period > 0.0 ? static_cast<double>(t_us) + period : -1.0;
        for (uint32_t i = 0; i < ms; i += 2) {
            t_us += 2000;
            while (next_pulse_us > 0.0 && next_pulse_us <= static_cast<double>(t_us)) {
                ++total;
                edge_us = static_cast<uint64_t>(next_pulse_us);
                next_pulse_us += period;
            }
            m.update(total, edge_us, t_us, c);
        }
    }
};
}  // namespace

int main() {
    CASE("the conversion itself");
    {
        // One pulse per rev, 100 pulses in one second, is 6000 rpm.
        CHECK_NEAR(rpm_from_pulses(100, 1000000, 1), 6000.0, 1e-3);
        CHECK_NEAR(rpm_from_pulses(100, 1000000, 2), 3000.0, 1e-3);
        CHECK_NEAR(rpm_from_pulses(0, 1000000, 1), 0.0, 1e-9);
        CHECK_NEAR(rpm_from_pulses(100, 0, 1), 0.0, 1e-9);
        CHECK_NEAR(rpm_from_pulses(100, 1000000, 0), 0.0, 1e-9);   // no divide by zero
    }

    CASE("a steady 6000 rpm reads 6000");
    {
        Ctrl x;
        x.run(6000.0f, 2000);
        CHECK_NEAR(x.m.rpm(), 6000.0, 120.0);
        CHECK(x.m.running());
    }

    CASE("a stopped engine reads zero, not a decaying number");
    {
        // The failure this exists to stop: an average over the last window
        // decays towards zero but never gets there, and the dash shows
        // 400 rpm on a bike that has been off for a minute.
        Ctrl x;
        x.run(6000.0f, 2000);
        CHECK(x.m.rpm() > 5000.0f);
        x.run(0.0f, 2000);
        CHECK_NEAR(x.m.rpm(), 0.0, 1e-6);
        CHECK(!x.m.running());
    }

    CASE("silence shorter than the stall timeout is not a stall");
    {
        // At 500 Hz there is no pulse in most ticks even at 6000 rpm.
        Ctrl x;
        x.run(1000.0f, 2000);
        CHECK(x.m.running());
        x.run(0.0f, 200);                 // 200 ms of quiet
        CHECK(x.m.running());
        CHECK(x.m.rpm() > 0.0f);
    }

    CASE("idle is followed, not smoothed into nothing");
    {
        Ctrl x;
        x.run(1400.0f, 3000);
        CHECK_NEAR(x.m.rpm(), 1400.0, 80.0);
    }

    CASE("a wrapped pulse counter does not spike the tach");
    {
        // uint32 of pulses at 6000 rpm wraps in about eight days of running.
        // A signed difference there reads as four billion pulses in 2 ms.
        Ctrl x;
        x.total = 0xFFFFFFF0u;
        x.run(6000.0f, 2000);
        CHECK(x.total < 0x1000u);         // it really did wrap
        CHECK_NEAR(x.m.rpm(), 6000.0, 200.0);
    }

    CASE("multiple pulses per revolution");
    {
        Ctrl x;
        x.c.pulses_per_rev = 2;
        x.run(4000.0f, 2000);
        CHECK_NEAR(x.m.rpm(), 4000.0, 100.0);
    }

    CASE("wheel speed from circumference and magnet count");
    {
        WheelConfig w;
        w.circumference_mm = 2130;
        w.pulses_per_rev = 6;
        // 6 pulses is one revolution, 2.13 m. Ten revolutions per second is
        // 21.3 m/s, which is 47.6 mph.
        CHECK_NEAR(mph_from_pulses(60, 1000000, w), 47.65, 0.05);
        CHECK_NEAR(mph_from_pulses(0, 1000000, w), 0.0, 1e-9);
    }

    CASE("a wheel that stops reads zero");
    {
        WheelConfig w;
        WheelMeter wm;
        uint32_t total = 0;
        uint64_t t = 0, edge = 0;
        for (int i = 0; i < 1000; ++i) {       // rolling
            t += 2000;
            if (i % 5 == 0) { ++total; edge = t; }
            wm.update(total, edge, t, w);
        }
        CHECK(wm.mph() > 0.0f);
        for (int i = 0; i < 2000; ++i) {       // stopped at the lights
            t += 2000;
            wm.update(total, edge, t, w);
        }
        CHECK_NEAR(wm.mph(), 0.0, 1e-6);
    }

    CASE("the front wheel is believed first, then the rear, then GPS");
    {
        // A driven rear wheel lies about speed under power, and this is a
        // dirt bike, so the front sensor wins whenever it is healthy.
        CHECK_NEAR(best_speed_mph(30.0f, true, 44.0f, true, 31.0f, true), 30.0, 1e-6);
        CHECK_NEAR(best_speed_mph(0.0f, false, 44.0f, true, 31.0f, true), 44.0, 1e-6);
        CHECK_NEAR(best_speed_mph(0.0f, false, 0.0f, false, 31.0f, true), 31.0, 1e-6);
        CHECK_NEAR(best_speed_mph(0.0f, false, 0.0f, false, 0.0f, false), 0.0, 1e-6);
    }

    return check::report("tach");
}
