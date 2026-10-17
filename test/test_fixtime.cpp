#include "check.hpp"
#include "core/fixtime/fixtime.hpp"
#include <cstdlib>

using core::fixtime::Clock;

namespace {

// A receiver at 10 Hz whose sentences arrive `base + 0..spread` ms after the
// fix, on a system clock `ppm` fast, starting at time of day `utc0`.
struct Receiver {
    uint32_t utc0 = 12u * 3600u * 1000u;
    uint32_t rx0 = 5000;
    int      base_ms = 40;
    int      spread_ms = 35;
    double   ppm = 0.0;
    uint32_t seed = 1;
    int      n = 0;

    uint32_t utc() const { return (utc0 + static_cast<uint32_t>(n) * 100u) % 86400000u; }
    // When the fix was really taken, on the system clock.
    double true_ms() const { return rx0 + n * 100.0 * (1.0 + ppm * 1e-6); }
    uint32_t rx() {
        seed = seed * 1103515245u + 12345u;
        const int lat = base_ms + static_cast<int>((seed >> 16) % static_cast<uint32_t>(spread_ms + 1));
        return static_cast<uint32_t>(true_ms() + lat);
    }
};

}  // namespace

int main() {
    CASE("jittery arrival, steady stamps: fixes 100 ms apart are stamped 100 ms apart");
    {
        Receiver r;
        Clock c;
        uint32_t prev = 0;
        int worst_rx = 0, worst_stamp = 0;
        uint32_t prev_rx = 0;
        for (; r.n < 600; ++r.n) {
            const uint32_t rx = r.rx();
            const uint32_t t = c.stamp(r.utc(), rx);
            CHECK(static_cast<int32_t>(rx - t) >= 0);          // never after it arrived
            if (r.n > 50) {    // a quick fix has been seen by now
                const int ds = std::abs(static_cast<int>(t - prev) - 100);
                const int dr = std::abs(static_cast<int>(rx - prev_rx) - 100);
                if (ds > worst_stamp) worst_stamp = ds;
                if (dr > worst_rx) worst_rx = dr;
            }
            prev = t;
            prev_rx = rx;
        }
        CHECK(worst_rx >= 25);        // the arrival times really were ragged
        CHECK(worst_stamp <= 1);      // the stamps are not
    }

    CASE("a lap's time is the GPS time between its fixes, not the arrival times'");
    {
        // Two crossings a minute apart, each fix arriving 40..75 ms late. The
        // lap timer takes differences of fix times; the error left is
        // rounding, not latency.
        Receiver r;
        Clock c;
        int worst = 0;
        for (; r.n < 3000; ++r.n) {
            const uint32_t utc_a = r.utc();
            const uint32_t a = c.stamp(utc_a, r.rx());
            if (r.n < 100 || r.n + 600 >= 3000) continue;
            // Look ahead a minute with a copy, so the main clock is untouched.
            Receiver r2 = r;
            Clock c2 = c;
            uint32_t b = 0;
            for (int k = 1; k <= 600; ++k) {
                ++r2.n;
                b = c2.stamp(r2.utc(), r2.rx());
            }
            const int err = std::abs(static_cast<int>(b - a) - 60000);
            if (err > worst) worst = err;
            r.n += 299;       // sample every 30 s
        }
        CHECK(worst <= 1);
    }

    CASE("a fast crystal is followed without letting a slow sentence in");
    {
        // The P4's crystal 50 ppm fast: 3 ms a minute. Over ten minutes the
        // stamps stay with the true time, less the receiver's quickest output.
        Receiver r;
        r.ppm = 50.0;
        Clock c;
        double worst = 0;
        for (; r.n < 6000; ++r.n) {
            const uint32_t t = c.stamp(r.utc(), r.rx());
            if (r.n < 100) continue;
            const double off = static_cast<double>(t) - r.true_ms() - r.base_ms;
            const double e = off < 0 ? -off : off;
            if (e > worst) worst = e;
        }
        CHECK(worst <= 3.0);
    }

    CASE("midnight: the time of day wraps and the stamps do not");
    {
        Receiver r;
        r.utc0 = 86400000u - 10000u;  // ten seconds before midnight
        Clock c;
        uint32_t prev = 0;
        int worst = 0;
        for (; r.n < 200; ++r.n) {
            const uint32_t t = c.stamp(r.utc(), r.rx());
            if (r.n > 50) {
                const int d = std::abs(static_cast<int>(t - prev) - 100);
                if (d > worst) worst = d;
            }
            prev = t;
        }
        CHECK(c.locked());
        CHECK(worst <= 1);
    }

    CASE("a receiver whose clock jumps is re-locked, not followed");
    {
        Receiver r;
        Clock c;
        for (; r.n < 50; ++r.n) c.stamp(r.utc(), r.rx());
        // The receiver's time of day jumps an hour (a cold start setting it).
        r.utc0 += 3600u * 1000u;
        const uint32_t rx = r.rx();
        const uint32_t t = c.stamp(r.utc(), rx);
        CHECK(static_cast<int32_t>(rx - t) >= 0);
        CHECK(rx - t <= 100);         // stamped near its arrival, not an hour off
        ++r.n;
        const uint32_t rx2 = r.rx();
        const uint32_t t2 = c.stamp(r.utc(), rx2);
        CHECK(t2 - t >= 50 && t2 - t <= 150);
    }

    CASE("the system clock wrapping at 49 days changes nothing");
    {
        Receiver r;
        r.rx0 = 0xFFFFFFFFu - 2000u;
        Clock c;
        uint32_t prev = 0;
        int worst = 0;
        for (; r.n < 200; ++r.n) {
            const uint32_t t = c.stamp(r.utc(), r.rx());
            if (r.n > 50) {
                const int d = std::abs(static_cast<int>(t - prev) - 100);
                if (d > worst) worst = d;
            }
            prev = t;
        }
        CHECK(worst <= 1);
    }

    CASE("a receiver with no time yet: fixes are stamped as they arrive");
    {
        Clock c;
        for (uint32_t rx = 1000; rx < 3000; rx += 100) {
            CHECK(c.stamp(0, rx) == rx);
        }
    }

    return check::report("fixtime");
}
