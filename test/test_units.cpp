#include "check.hpp"
#include "core/units/units.hpp"
#include <cstring>

using namespace core::units;

namespace {
// Every formatter writes into a caller buffer, so the tests all look like
// this. 32 bytes is more than any of them need.
struct Buf {
    char b[32];
    const char* operator()(size_t len) { (void)len; return b; }
};
bool eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
}  // namespace

int main() {
    CASE("conversions round-trip");
    CHECK_NEAR(mph_from_kmh(kmh_from_mph(60.0f)), 60.0, 1e-4);
    CHECK_NEAR(c_from_f(f_from_c(21.5f)), 21.5, 1e-4);
    CHECK_NEAR(ft_from_m(m_from_ft(100.0f)), 100.0, 1e-3);
    CHECK_NEAR(psi_from_bar(bar_from_psi(32.0f)), 32.0, 1e-4);
    CHECK_NEAR(gal_from_l(l_from_gal(1.6f)), 1.6, 1e-5);

    CASE("conversions against known values");
    CHECK_NEAR(kmh_from_mph(60.0f), 96.56, 0.01);
    CHECK_NEAR(f_from_c(100.0f), 212.0, 1e-4);
    CHECK_NEAR(f_from_c(0.0f), 32.0, 1e-4);
    CHECK_NEAR(mph_from_ftps(88.0f), 60.0, 0.01);      // 88 ft/s is 60 mph
    CHECK_NEAR(mi_from_ft(5280.0f), 1.0, 1e-6);

    CASE("display conversion reports the unit label with the number");
    const char* u = nullptr;
    CHECK_NEAR(speed_for_display(60.0f, System::Imperial, &u), 60.0, 1e-4);
    CHECK(eq(u, "mph"));
    CHECK_NEAR(speed_for_display(60.0f, System::Metric, &u), 96.56, 0.01);
    CHECK(eq(u, "km/h"));
    CHECK_NEAR(temp_for_display(100.0f, System::Imperial, &u), 212.0, 1e-3);
    CHECK(eq(u, "F"));

    Buf b;

    CASE("fixed-point formatting rounds, and does not invent a minus zero");
    fmt_float(b.b, sizeof b.b, 12.345f, 2);   CHECK(eq(b.b, "12.35"));
    fmt_float(b.b, sizeof b.b, 12.344f, 2);   CHECK(eq(b.b, "12.34"));
    fmt_float(b.b, sizeof b.b, -12.345f, 1);  CHECK(eq(b.b, "-12.3"));
    fmt_float(b.b, sizeof b.b, 0.0f, 0);      CHECK(eq(b.b, "0"));
    fmt_float(b.b, sizeof b.b, 99.96f, 1);    CHECK(eq(b.b, "100.0"));
    fmt_float(b.b, sizeof b.b, 7.0f, 0);      CHECK(eq(b.b, "7"));
    // -0.04 at one decimal is zero. Printing "-0.0" on a dashboard looks
    // like a fault in the sensor rather than a value near zero.
    fmt_float(b.b, sizeof b.b, -0.04f, 1);    CHECK(eq(b.b, "0.0"));

    CASE("a value from a device that is not Ok renders as dashes");
    fmt_value(b.b, sizeof b.b, 32.0f, 1, true);   CHECK(eq(b.b, "32.0"));
    fmt_value(b.b, sizeof b.b, 32.0f, 1, false);  CHECK(eq(b.b, "--"));
    // Not "0.0" — a zero and a dead sensor must never look the same.
    fmt_value(b.b, sizeof b.b, 0.0f, 1, false);   CHECK(eq(b.b, "--"));

    CASE("lap times");
    fmt_duration(b.b, sizeof b.b, 83450, true);    CHECK(eq(b.b, "1:23.45"));
    fmt_duration(b.b, sizeof b.b, 9990, true);     CHECK(eq(b.b, "0:09.99"));
    fmt_duration(b.b, sizeof b.b, 600000, true);   CHECK(eq(b.b, "10:00.00"));
    fmt_duration(b.b, sizeof b.b, 3723400, true);  CHECK(eq(b.b, "1:02:03.4"));
    // An unset best lap is not 0:00.00.
    fmt_duration(b.b, sizeof b.b, 0, false);       CHECK(eq(b.b, "--:--.--"));

    CASE("deltas always carry their sign, at one decimal by default");
    // One decimal is the honest resolution of a GPS-matched lap delta; see
    // the note on fmt_delta. The sign is drawn whatever the magnitude,
    // because a rider reads the colour and the sign before the digits.
    fmt_delta(b.b, sizeof b.b, 420, true);    CHECK(eq(b.b, "+0.4"));
    fmt_delta(b.b, sizeof b.b, -1080, true);  CHECK(eq(b.b, "-1.1"));
    fmt_delta(b.b, sizeof b.b, 0, true);      CHECK(eq(b.b, "+0.0"));
    fmt_delta(b.b, sizeof b.b, 0, false);     CHECK(eq(b.b, "--"));
    // Two, when the question is one measured lap time against another.
    fmt_delta(b.b, sizeof b.b, 420, true, 2); CHECK(eq(b.b, "+0.42"));

    CASE("clock applies the zone offset and wraps at midnight");
    const uint32_t t_1330_utc = (13 * 3600 + 30 * 60) * 1000u;
    fmt_clock(b.b, sizeof b.b, t_1330_utc, 0, true);    CHECK(eq(b.b, "13:30"));
    fmt_clock(b.b, sizeof b.b, t_1330_utc, -5, true);   CHECK(eq(b.b, "08:30"));
    fmt_clock(b.b, sizeof b.b, t_1330_utc, -14, true);  CHECK(eq(b.b, "23:30"));
    fmt_clock(b.b, sizeof b.b, t_1330_utc, 11, true);   CHECK(eq(b.b, "00:30"));
    fmt_clock(b.b, sizeof b.b, 0, 0, false);            CHECK(eq(b.b, "--:--"));

    CASE("a short buffer truncates and still terminates");
    char small[4];
    size_t n = fmt_float(small, sizeof small, 123.456f, 2);
    CHECK(n == 3);
    CHECK(small[3] == '\0');
    CHECK(eq(small, "123"));
    CASE("rpm on screen is to the nearest ten");
    CHECK_NEAR(rpm_for_display(4123.0f), 4120.0, 1e-3);
    CHECK_NEAR(rpm_for_display(4125.0f), 4130.0, 1e-3);
    CHECK_NEAR(rpm_for_display(0.0f), 0.0, 1e-6);

    // Zero-length buffers are not a crash.
    CHECK(fmt_float(nullptr, 0, 1.0f, 0) == 0);

    return check::report("units");
}
