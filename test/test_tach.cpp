#include "check.hpp"
#include <algorithm>
#include <cmath>
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

    CASE("a spark every other revolution reads double per pulse");
    {
        // The same pulse train as 2000 rpm at one spark a revolution.
        Ctrl x;
        x.c.revs_per_pulse = 2;
        x.run(2000.0f, 2000);
        CHECK_NEAR(x.m.raw_rpm(), 4000.0, 50.0);
    }

    CASE("the rate of climb is the slope of a steady pull");
    {
        // 4000 rpm rising at 6000 rpm/s, one edge per revolution, through the
        // Meter exactly as ctrl feeds it.
        Meter m;
        Config c;
        double rev = 0.0, t_s = 0.0;
        uint32_t total = 0;
        uint64_t edge_us = 0;
        for (int i = 0; i < 400; ++i) {                     // 0.4 s of 1 ms steps
            for (int k = 0; k < 100; ++k) {                 // 10 us integration
                const double rpm = 4000.0 + 6000.0 * t_s;
                rev += rpm / 60.0 * 10e-6;
                t_s += 10e-6;
                if (rev >= 1.0) { rev -= 1.0; ++total; edge_us = static_cast<uint64_t>(t_s * 1e6); }
            }
            m.update(total, edge_us, static_cast<uint64_t>(t_s * 1e6), c);
        }
        CHECK_NEAR(m.rpm_rate(), 6000.0, 300.0);
    }

    CASE("a steady engine with a jittery period reads a small rate");
    {
        // A percent of period jitter is 60 rpm per reading at 6000 rpm. A
        // two-point derivative would call that +-6000 rpm/s.
        RateEstimator r;
        uint32_t seed = 12345;
        uint64_t t = 0;
        float worst = 0.0f;
        for (int i = 0; i < 500; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float jitter = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.02f;
            const float period_us = 10000.0f * (1.0f + jitter);
            t += static_cast<uint64_t>(period_us);
            r.add(t, 60000000.0f / period_us, 100000);
            if (i > 20) worst = std::max(worst, std::abs(r.rpm_per_s()));
        }
        CHECK(worst < 1500.0f);
    }

    CASE("the rate is zero until there are readings, and after a stall");
    {
        RateEstimator r;
        CHECK(r.rpm_per_s() == 0.0f);
        r.add(0, 5000.0f, 100000);
        r.add(10000, 5100.0f, 100000);
        CHECK(r.rpm_per_s() == 0.0f);                       // two is not a slope
        r.add(20000, 5200.0f, 100000);
        CHECK_NEAR(r.rpm_per_s(), 10000.0, 1.0);

        Ctrl x;
        x.run(4000.0f, 100);
        x.run(7000.0f, 100);                                 // a blip
        x.run(0.0f, 1000);                                   // and it stalls
        CHECK(!x.m.running());
        CHECK(x.m.rpm_rate() == 0.0f);
    }

    CASE("a glitch cannot claim an impossible rate");
    {
        RateEstimator r;
        r.add(0, 3000.0f, 100000);
        r.add(1000, 3000.0f, 100000);
        r.add(2000, 60000.0f, 100000);                       // one wild reading
        CHECK(r.rpm_per_s() <= RateEstimator::kMaxRpmPerS);
    }

    CASE("pulses with no usable edge time wait for one, not vanish");
    {
        // 6000 rpm: an edge every 10 ms. One read comes back with its count
        // advanced but a timestamp that is no newer -- the driver's torn read
        // -- and the next good read must still say 6000, not 3000.
        Meter m;
        Config c;
        c.smoothing = 1.0f;
        uint32_t total = 0;
        uint64_t edge = 1000000;
        m.update(total, edge, edge, c);
        for (int i = 0; i < 5; ++i) {
            ++total;
            edge += 10000;
            m.update(total, edge, edge + 500, c);
        }
        CHECK_NEAR(m.raw_rpm(), 6000.0f, 1.0f);
        ++total;                                  // a pulse at edge + 10 ms...
        m.update(total, edge - 3000, edge + 10500, c);   // ...stamped in the past
        CHECK_NEAR(m.raw_rpm(), 6000.0f, 1.0f);   // held, not a new reading
        ++total;
        edge += 20000;
        m.update(total, edge, edge + 500, c);     // two pulses over two periods
        CHECK_NEAR(m.raw_rpm(), 6000.0f, 1.0f);
    }
    {
        WheelMeter w;
        WheelConfig c;
        uint32_t total = 0;
        uint64_t edge = 1000000;
        w.update(total, edge, edge, c);
        // 6 magnets, 2130 mm: 20 ms between pulses is ~39.7 mph.
        const float want = mph_from_pulses(1, 20000, c);
        for (int i = 0; i < 40; ++i) {
            ++total;
            edge += 20000;
            w.update(total, edge, edge + 500, c);
        }
        CHECK_NEAR(w.mph(), want, 0.1f);
        ++total;
        w.update(total, 0, edge + 20500, c);      // count moved, time did not
        ++total;
        edge += 40000;
        w.update(total, edge, edge + 500, c);
        CHECK_NEAR(w.mph(), want, 0.1f);
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

    // A wheel turning at `mph` for `ms`, polled every 2 ms like `ctrl`.
    struct Wheel {
        WheelConfig c;
        WheelMeter m;
        uint32_t total = 0;
        uint64_t t = 0, edge = 0;
        double next_edge = 0;
        void run(float mph, int ms) {
            const double per_pulse_mm = double(c.circumference_mm) / c.pulses_per_rev;
            const double mm_per_us = mph * 1609344.0 / 3600e6;
            for (int i = 0; i < ms / 2; ++i) {
                t += 2000;
                if (mph > 0) {
                    while (next_edge <= double(t)) {
                        ++total;
                        edge = static_cast<uint64_t>(next_edge);
                        next_edge += per_pulse_mm / mm_per_us;
                    }
                } else {
                    next_edge = double(t) + 1e12;
                }
                m.update(total, edge, t, c);
            }
        }
    };

    CASE("wheel smoothing lags by about tau at any speed, not by pulses");
    {
        // At 5 mph a pulse is ~160 ms apart. Per pulse, 1.5 pulses of lag was
        // ~240 ms; in time, a step is mostly followed within two pulses.
        Wheel w;
        w.next_edge = 1000;
        w.run(5.0f, 3000);
        CHECK_NEAR(w.m.mph(), 5.0, 0.1);
        w.run(8.0f, 400);            // two or three pulses at the new speed
        CHECK(w.m.mph() > 7.5f);
        // At 60 mph the same constant still smooths (pulses ~13 ms apart).
        Wheel f;
        f.next_edge = 1000;
        f.run(60.0f, 2000);
        CHECK_NEAR(f.m.mph(), 60.0, 0.5);
        f.run(70.0f, 40);            // two or three pulses at the new speed
        CHECK(f.m.mph() < 69.0f);     // not a step to the new value
        CHECK(f.m.mph() > 60.5f);
    }

    CASE("a wheel that stops reads less as soon as its pulses are late");
    {
        Wheel w;
        w.next_edge = 1000;
        w.run(5.0f, 3000);
        const float rolling = w.m.mph();
        w.run(0.0f, 500);            // stopped: no more edges
        // 500 ms after the last edge, one pulse in that time is ~1.6 mph.
        CHECK(w.m.mph() < 0.5f * rolling);
        CHECK(w.m.mph() > 0.0f);
        w.run(0.0f, 1200);           // past stall_ms: exactly zero
        CHECK_NEAR(w.m.mph(), 0.0, 1e-6);
    }

    CASE("a steady wheel does not dip between pulses");
    {
        Wheel w;
        w.next_edge = 1000;
        w.run(20.0f, 2000);
        float lo = 1e9f, hi = 0.0f;
        for (int i = 0; i < 200; ++i) {
            w.run(20.0f, 2);
            lo = std::min(lo, w.m.mph());
            hi = std::max(hi, w.m.mph());
        }
        CHECK(lo > 19.8f);
        CHECK(hi < 20.2f);
    }

    return check::report("tach");
}
