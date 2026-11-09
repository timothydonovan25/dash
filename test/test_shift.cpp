#include "check.hpp"
#include "core/shift/shift.hpp"
#include "core/tach/tach.hpp"
using namespace core::shift;

int main() {
    Config c;   // base 6000, span 2400, hyst 120, flash 8800

    CASE("all lamps dark below base");
    CHECK(pattern(5000, c, 0) == 0x00);
    CHECK(pattern(5999, c, 0) == 0x00);

    CASE("first lamp lights exactly at base");
    CHECK(pattern(6000, c, 0) == 0x01);

    CASE("all eight lit at base + span and above");
    CHECK(pattern(8400, c, 0) == 0xFF);
    CHECK(pattern(9999, c, 0) == 0xFF);

    CASE("thresholds are evenly spaced and monotonic");
    CHECK(threshold(c, 0) == 6000);
    CHECK(threshold(c, 7) == 8400);
    for (int i = 1; i < kLampCount; ++i)
        CHECK(threshold(c, i) > threshold(c, i - 1));

    CASE("lamp count rises monotonically with rpm");
    int last = 0;
    for (uint16_t rpm = 5500; rpm <= 8800; rpm += 25) {
        int n = 0;
        for (uint8_t m = pattern(rpm, c, 0); m; m >>= 1) n += (m & 1);
        CHECK(n >= last);
        last = n;
    }

    CASE("hysteresis holds a lit lamp through a small dip");
    uint8_t m = pattern(6000, c, 0);
    CHECK(m == 0x01);
    m = pattern(5950, c, m);          // 50 below, inside the 120 band
    CHECK(m == 0x01);                 // stays lit
    m = pattern(5850, c, m);          // 150 below, outside the band
    CHECK(m == 0x00);                 // drops

    CASE("no flicker when rpm sits exactly on a threshold");
    const uint16_t t3 = threshold(c, 3);
    uint8_t prev = pattern(t3, c, 0);
    for (int i = 0; i < 50; ++i) {
        const uint16_t rpm = static_cast<uint16_t>(t3 + ((i % 2) ? 1 : -1));
        const uint8_t next = pattern(rpm, c, prev);
        CHECK(next == prev);          // this is the whole point of hysteresis
        prev = next;
    }

    CASE("over_limit trips at flash_rpm");
    CHECK(!over_limit(8799, c));
    CHECK(over_limit(8800, c));

    CASE("hysteresis larger than a threshold clamps instead of wrapping");
    // Lamps whose threshold is below the hysteresis band clamp their bar
    // to 0 and stay lit at 0 rpm; the rest drop out normally. The bug this
    // guards against is uint16 underflow turning a low bar into ~65000.
    Config odd; odd.base_rpm = 50; odd.hysteresis_rpm = 500;
    CHECK(pattern(0, odd, 0xFF) == 0x03);

    CASE("lamps always light in order, never with a gap");
    // The mask must always be 2^n - 1. A gap means the threshold maths
    // went non-monotonic somewhere.
    for (uint16_t rpm = 0; rpm < 12000; rpm += 17) {
        for (uint8_t prev : {uint8_t{0x00}, uint8_t{0x0F}, uint8_t{0xFF}}) {
            const uint8_t m = pattern(rpm, c, prev);
            CHECK((m & static_cast<uint8_t>(m + 1)) == 0);
        }
    }

    CASE("lead time off: the lamps see the rpm as it is");
    {
        Config d;                                  // lead_ms 0
        CHECK(lamp_rpm(7000.0f, 8000.0f, d) == 7000);
        CHECK(lamp_rpm(7000.0f, -8000.0f, d) == 7000);
        CHECK(lamp_rpm(-5.0f, 0.0f, d) == 0);
    }

    CASE("lead time on: a climbing engine is shown where it is heading");
    {
        Config d;
        d.lead_ms = 100;
        // (5000 - the 1000 rpm/s noise floor) for 0.1 s
        CHECK(lamp_rpm(7000.0f, 5000.0f, d) == 7400);
        CHECK(lamp_rpm(7000.0f, 0.0f, d) == 7000);      // steady
        CHECK(lamp_rpm(7000.0f, 900.0f, d) == 7000);    // jitter, under the floor
        CHECK(lamp_rpm(7000.0f, 1100.0f, d) == 7010);   // no step at the floor
        CHECK(lamp_rpm(7000.0f, -9000.0f, d) == 7000);  // falling: as it is
        CHECK(lamp_rpm(0.0f, 5000.0f, d) == 0);         // not running
        d.lead_ms = 300;
        CHECK(lamp_rpm(7000.0f, 30000.0f, d) == 8000);  // capped at +1000
        d.lead_floor_rpm_s = 0;
        d.lead_ms = 100;
        CHECK(lamp_rpm(7000.0f, 5000.0f, d) == 7500);   // floor off: the whole rate
    }

    CASE("a 150 ms lead flashes about 150 ms of climb early on a hard pull");
    // ...less the noise floor.
    {
        // Through the real tach: edges from an engine pulling 6000 rpm/s,
        // the Meter's rate, lamp_rpm, then the flash test ctrl makes.
        auto flash_at = [](uint16_t lead_ms) {
            core::tach::Meter m;
            core::tach::Config tc;
            Config sc;
            sc.lead_ms = lead_ms;
            double rev = 0.0, t_s = 0.0;
            uint32_t total = 0;
            uint64_t edge_us = 0;
            for (int i = 0; i < 1000; ++i) {               // 1 s in 1 ms ticks
                double rpm = 0.0;
                for (int k = 0; k < 100; ++k) {
                    rpm = 4000.0 + 6000.0 * t_s;
                    rev += rpm / 60.0 * 10e-6;
                    t_s += 10e-6;
                    if (rev >= 1.0) { rev -= 1.0; ++total; edge_us = static_cast<uint64_t>(t_s * 1e6); }
                }
                m.update(total, edge_us, static_cast<uint64_t>(t_s * 1e6), tc);
                if (over_limit(lamp_rpm(m.raw_rpm(), m.rpm_rate(), sc), sc)) return rpm;
            }
            return 0.0;
        };
        const double on_time = flash_at(0);
        const double early = flash_at(150);
        CHECK_NEAR(on_time, 8800.0, 150.0);        // the reading lags a revolution
        CHECK_NEAR(on_time - early, 750.0, 200.0); // (6000 - 1000) rpm/s * 0.15 s
    }

    CASE("a shift leaves the peak marked for the hold, then lets it go");
    {
        core::shift::PeakHold h;
        uint32_t t = 1000;
        float mark = 0.0f;
        // Up through second to 9000: nothing marked, the mark would be on the needle.
        for (float rpm = 4000; rpm <= 9000; rpm += 100, t += 10) {
            mark = h.update(rpm, t, 2000);
            CHECK(mark == 0.0f);
        }
        // The shift: down to 6500 in a few passes.
        for (float rpm = 8500; rpm >= 6500; rpm -= 500, t += 10) mark = h.update(rpm, t, 2000);
        CHECK(mark == 9000.0f);
        // Pulling again in third: still 9000 while the hold lasts...
        for (float rpm = 6500; rpm <= 7500; rpm += 50, t += 10) mark = h.update(rpm, t, 2000);
        CHECK(mark == 9000.0f);
        // ...gone when it runs out, and back to following.
        t += 2000;
        CHECK(h.update(7600, t, 2000) == 0.0f);
        CHECK(h.update(7700, t + 10, 2000) == 0.0f);
    }

    CASE("catching up with the held peak takes it over; letting go follows down");
    {
        core::shift::PeakHold h;
        uint32_t t = 1000;
        h.update(8000, t, 2000);
        CHECK(h.update(7000, t += 10, 2000) == 8000.0f);
        // Back up past it within the hold: the new peak, marked again only
        // when the rpm drops away from that.
        CHECK(h.update(8200, t += 10, 2000) == 0.0f);
        CHECK(h.update(7600, t += 10, 2000) == 8200.0f);
        // Rolling off after the hold: no fresh mark at every lower rpm on the
        // way down.
        t += 2100;
        for (float rpm = 7500; rpm >= 2000; rpm -= 100) {
            CHECK(h.update(rpm, t += 10, 2000) == 0.0f);
        }
        // A drop under the gap is not marked: noise, or a gentle change.
        h.reset();
        h.update(6000, t += 10, 2000);
        CHECK(h.update(5700, t += 10, 2000) == 0.0f);
        // And 0 ms is off.
        CHECK(h.update(9000, t += 10, 0) == 0.0f);
        CHECK(h.update(5000, t += 10, 0) == 0.0f);
    }

    CASE("the held lamp is the highest the peak reached");
    {
        core::shift::Config c;   // lamps from 6000 every 300 (2400 / 8), flash 8800
        CHECK(core::shift::hold_lamp(5900, c) == 0);
        CHECK(core::shift::hold_lamp(6000, c) == 0x01);
        CHECK(core::shift::hold_lamp(core::shift::threshold(c, 3), c) == 0x08);
        CHECK(core::shift::hold_lamp(core::shift::threshold(c, 7), c) == 0x80);
        CHECK(core::shift::hold_lamp(9500, c) == 0x80);
    }

    CASE("over the limit the lamps blink, starting lit");
    {
        core::shift::Config c;   // flash 8800, period 125 ms
        core::shift::Flasher f;
        // Under the limit the mask goes through as it is.
        CHECK(f.apply(0x0F, false, 1000, c) == 0x0F);
        // Crossing: lit at once, whatever the mask was.
        CHECK(f.apply(0xFF, true, 1003, c) == 0xFF);
        CHECK(f.apply(0xFF, true, 1003 + 61, c) == 0xFF);
        CHECK(f.apply(0xFF, true, 1003 + 62, c) == 0x00);
        CHECK(f.apply(0xFF, true, 1003 + 124, c) == 0x00);
        CHECK(f.apply(0xFF, true, 1003 + 125, c) == 0xFF);
        // Held over for a second: lit half the time, not stuck either way.
        int lit = 0;
        for (uint32_t t = 1003; t < 2003; t += 2) lit += f.apply(0xFF, true, t, c) == 0xFF;
        CHECK(lit >= 245 && lit <= 255);   // 62 of each odd 125
        // Dropping under ends it, and the next crossing starts lit again
        // even mid-way through what would have been a dark half.
        CHECK(f.apply(0x7F, false, 2050, c) == 0x7F);
        CHECK(f.apply(0xFF, true, 2100, c) == 0xFF);
        // Across the clock's wrap.
        core::shift::Flasher w;
        CHECK(w.apply(0xFF, true, 0xFFFFFFF0u, c) == 0xFF);
        CHECK(w.apply(0xFF, true, 0xFFFFFFF0u + 70u, c) == 0x00);
        // A period of 0 is solid.
        c.flash_period_ms = 0;
        CHECK(w.apply(0xFF, true, 5, c) == 0xFF);
    }

    // The PCA9955B's fault flags, laid out as the board has them: lamps on
    // OUT0..7, backlight on OUT8..10.
    {
        using core::shift::decode_led_faults;
        using core::shift::LedFaults;
        const uint8_t clean[4] = {};
        CHECK(decode_led_faults(0x01, clean, 0, 8, 8, 3) == LedFaults{});  // reserved bit 0 reads 1
        // Lamp 1 short (OUT0 = 01b), lamp 4 open (OUT3 = 10b), lamp 8 open (OUT7).
        const uint8_t lamps[4] = {0b10000001, 0b10000000, 0, 0};
        LedFaults f = decode_led_faults(0x40, lamps, 0, 8, 8, 3);
        CHECK(f.lamp_short == 0x01);
        CHECK(f.lamp_open == 0x88);
        CHECK(f.backlight_open == 0 && f.backlight_short == 0 && !f.overtemp);
        // Backlight string 2 (OUT9) open, string 3 (OUT10) short; the
        // spare outputs above (OUT11..15) and 11b are never reported.
        const uint8_t back[4] = {0, 0, 0b11011000, 0xFF};
        f = decode_led_faults(0xC0, back, 0, 8, 8, 3);
        CHECK(f.backlight_open == 0x02);
        CHECK(f.backlight_short == 0x04);
        CHECK(f.lamp_open == 0 && f.lamp_short == 0);
        CHECK(f.overtemp);
        CHECK(f != LedFaults{});
    }

    return check::report("shift");
}
