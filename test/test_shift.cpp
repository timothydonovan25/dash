#include "check.hpp"
#include "core/shift/shift.hpp"
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

    return check::report("shift");
}
