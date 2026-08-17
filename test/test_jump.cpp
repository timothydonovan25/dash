#include "check.hpp"
#include "core/jump/jump.hpp"

using namespace core::jump;

namespace {
// Feed a constant magnitude for a span at 200 Hz, the rate the IMU FIFO is
// drained at. Returns the last event seen.
Update run(Detector& d, const Config& c, float mag_g, uint32_t ms,
           uint32_t& t_ms, Update* landed = nullptr) {
    Update last;
    for (uint32_t i = 0; i < ms; i += 5) {
        t_ms += 5;
        const Update u = d.update(mag_g, t_ms, c);
        if (u.event != Event::None) {
            last = u;
            if (landed && u.event == Event::Landed) *landed = u;
        }
    }
    return last;
}
}  // namespace

int main() {
    const Config cfg;

    CASE("a jump: free fall, then a landing peak");
    {
        Detector d;
        uint32_t t = 0;
        Update landed;
        run(d, cfg, 1.0f, 1000, t);
        CHECK(!d.airborne());

        run(d, cfg, 0.05f, 800, t);          // 0.8 s in the air
        CHECK(d.airborne());
        CHECK_NEAR(d.airtime_ms(), 800.0, 10.0);

        // Landing: crosses 2 g, peaks at 4.5 g a few samples later.
        run(d, cfg, 2.2f, 20, t, &landed);
        run(d, cfg, 4.5f, 30, t, &landed);
        run(d, cfg, 1.0f, 300, t, &landed);
        CHECK(!d.airborne());
        CHECK(landed.event == Event::Landed);
        CHECK_NEAR(landed.airtime_ms, 800.0, 15.0);
        // The peak, not the 2.2 g sample that tripped the threshold.
        CHECK_NEAR(landed.landing_g, 4.5, 0.01);
        CHECK_NEAR(d.last_landing_g(), 4.5, 0.01);
        CHECK_NEAR(d.best_ms(), 800.0, 15.0);
    }

    CASE("airtime counts from when the wheels left, not from when we were sure");
    {
        // The 100 ms qualifying window is evidence that a jump happened. It
        // is not part of the jump, and charging it to the rider would lose
        // a tenth off every airtime.
        Detector d;
        uint32_t t = 0;
        run(d, cfg, 1.0f, 500, t);
        run(d, cfg, 0.1f, 110, t);           // just past the qualifying window
        CHECK(d.airborne());
        // The moment it qualifies, the clock already reads the full window
        // rather than starting from zero.
        CHECK_NEAR(d.airtime_ms(), 105.0, 10.0);
    }

    CASE("a whoop is not a jump");
    {
        // The suspension unloads over a bump for 60 ms. Riders hit dozens of
        // these a lap and none of them are air.
        Detector d;
        uint32_t t = 0;
        for (int i = 0; i < 20; ++i) {
            run(d, cfg, 1.0f, 200, t);
            run(d, cfg, 0.1f, 60, t);
            CHECK(!d.airborne());
        }
        CHECK(d.best_ms() == 0);
    }

    CASE("a rough landing that dips again does not score a second jump");
    {
        Detector d;
        uint32_t t = 0;
        Update landed;
        run(d, cfg, 1.0f, 200, t);
        run(d, cfg, 0.05f, 600, t);
        run(d, cfg, 3.0f, 40, t, &landed);
        run(d, cfg, 0.2f, 60, t, &landed);   // rebound: brief and unloaded
        run(d, cfg, 1.0f, 400, t, &landed);
        CHECK(!d.airborne());
        CHECK_NEAR(landed.airtime_ms, 600.0, 15.0);
        CHECK(d.best_ms() < 700);            // only the one jump
    }

    CASE("best airtime keeps the longest of the session");
    {
        Detector d;
        uint32_t t = 0;
        for (uint32_t ms : {400u, 1200u, 700u}) {
            run(d, cfg, 1.0f, 300, t);
            run(d, cfg, 0.05f, ms, t);
            run(d, cfg, 3.0f, 200, t);
        }
        CHECK_NEAR(d.best_ms(), 1200.0, 15.0);
    }

    CASE("an implausible airtime is dropped, not published");
    {
        // A dead IMU that reads zero looks exactly like an eight-second
        // jump. Publishing a record airtime because the sensor fell off is
        // worse than publishing nothing.
        Detector d;
        uint32_t t = 0;
        run(d, cfg, 1.0f, 200, t);
        run(d, cfg, 0.0f, 5000, t);
        CHECK(d.airborne());
        run(d, cfg, 0.0f, 4000, t);
        CHECK(!d.airborne());
        CHECK(d.best_ms() == 0);             // never recorded

        // And it does not start the cycle again while the sensor is still
        // reading zero — otherwise the dash declares the bike airborne
        // every eight seconds for the rest of the ride.
        run(d, cfg, 0.0f, 20000, t);
        CHECK(!d.airborne());
        CHECK(d.best_ms() == 0);

        // Once the sensor produces something plausible, it works again.
        run(d, cfg, 1.0f, 500, t);
        run(d, cfg, 0.05f, 600, t);
        CHECK(d.airborne());
    }

    CASE("the published attitude carries the live airtime");
    {
        Detector d;
        uint32_t t = 0;
        run(d, cfg, 1.0f, 200, t);
        run(d, cfg, 0.05f, 500, t);
        core::state::Attitude a;
        d.publish_into(a);
        CHECK(a.airborne);
        CHECK_NEAR(a.airtime_ms, 500.0, 10.0);
    }

    CASE("the landing is timestamped, so a screen can hold it and let go");
    {
        Detector d;
        uint32_t t = 0;

        // Nothing has happened yet, and that must not look like a landing at
        // time zero — a screen showing "0.00 s" from boot until the first
        // jump would be worse than showing nothing.
        core::state::Attitude a;
        d.publish_into(a);
        CHECK(a.land_ms == 0);

        run(d, cfg, 1.0f, 200, t);
        run(d, cfg, 0.05f, 800, t);          // in the air
        run(d, cfg, 5.0f, 300, t);           // down, and past the impact window
        d.publish_into(a);

        CHECK(a.land_ms != 0);
        CHECK(!a.airborne);
        // Reported when the peak is known, which is one impact window after
        // touchdown rather than at it.
        CHECK_NEAR(a.land_ms, 1000.0 + cfg.impact_window_ms, 20.0);
        CHECK_NEAR(a.airtime_ms, 800.0, 20.0);
        CHECK_NEAR(a.last_land_g, 5.0, 0.01);

        // It stays put afterwards: the hold is the screen's to time, and a
        // timestamp that crept forward would never expire.
        const uint32_t landed_at = a.land_ms;
        run(d, cfg, 1.0f, 4000, t);
        d.publish_into(a);
        CHECK(a.land_ms == landed_at);
    }

    return check::report("jump");
}
