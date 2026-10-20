#include "core/units/units.hpp"
#include <cmath>

namespace core::units {
namespace {

// Everything here formats with integer maths rather than snprintf("%f").
//
// Two reasons. The render task walks every widget at 60 Hz and this is on
// that path. More importantly, ESP-IDF can be built against newlib-nano,
// where `%f` silently prints nothing at all — a dashboard that shows blank
// speeds only on a release build is a bad afternoon. Integer digits cannot
// do that to you.

constexpr int64_t kPow10[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};

size_t emit(char* buf, size_t n, const char* src, size_t len) {
    if (n == 0) return 0;
    size_t w = len < n - 1 ? len : n - 1;
    for (size_t i = 0; i < w; ++i) buf[i] = src[i];
    buf[w] = '\0';
    return w;
}

size_t emit_lit(char* buf, size_t n, const char* s) {
    size_t len = 0;
    while (s[len]) ++len;
    return emit(buf, n, s, len);
}

// Fixed-point digits of |v| into tmp, with sign. Returns the length.
size_t render_fixed(char* tmp, float v, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;

    const double scaled = static_cast<double>(v) * static_cast<double>(kPow10[decimals]);
    int64_t iv = static_cast<int64_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    const bool neg = iv < 0;
    if (neg) iv = -iv;

    int64_t ip = iv / kPow10[decimals];
    const int64_t fp = iv % kPow10[decimals];

    char rev[24];
    size_t r = 0;
    if (ip == 0) rev[r++] = '0';
    while (ip > 0 && r < sizeof(rev)) {
        rev[r++] = static_cast<char>('0' + static_cast<int>(ip % 10));
        ip /= 10;
    }

    size_t k = 0;
    // Not "-0.0": a value that rounds to zero has no sign.
    if (neg && iv != 0) tmp[k++] = '-';
    while (r > 0) tmp[k++] = rev[--r];
    if (decimals > 0) {
        tmp[k++] = '.';
        for (int d = decimals - 1; d >= 0; --d) {
            const int64_t digit = (fp / kPow10[d]) % 10;
            tmp[k++] = static_cast<char>('0' + static_cast<int>(digit));
        }
    }
    return k;
}

size_t two_digits(char* out, uint32_t v) {
    out[0] = static_cast<char>('0' + (v / 10) % 10);
    out[1] = static_cast<char>('0' + v % 10);
    return 2;
}

}  // namespace

float rpm_for_display(float rpm) {
    if (!std::isfinite(rpm)) return rpm;
    return std::round(rpm / 10.0f) * 10.0f;
}

float speed_for_display(float mph, System s, const char** unit) {
    if (s == System::Metric) { if (unit) *unit = "km/h"; return kmh_from_mph(mph); }
    if (unit) *unit = "mph";
    return mph;
}

float distance_for_display(float mi, System s, const char** unit) {
    if (s == System::Metric) { if (unit) *unit = "km"; return km_from_mi(mi); }
    if (unit) *unit = "mi";
    return mi;
}

float temp_for_display(float c, System s, const char** unit) {
    if (s == System::Metric) { if (unit) *unit = "C"; return c; }
    if (unit) *unit = "F";
    return f_from_c(c);
}

float pressure_for_display(float psi, System s, const char** unit) {
    if (s == System::Metric) { if (unit) *unit = "bar"; return bar_from_psi(psi); }
    if (unit) *unit = "psi";
    return psi;
}

float volume_for_display(float gal, System s, const char** unit) {
    if (s == System::Metric) { if (unit) *unit = "L"; return l_from_gal(gal); }
    if (unit) *unit = "gal";
    return gal;
}

size_t fmt_float(char* buf, size_t n, float v, int decimals) {
    if (n == 0) return 0;
    if (!std::isfinite(v)) return emit_lit(buf, n, "--");
    // render_fixed scales into an int64_t, and a float past it (1e19 at no
    // decimals, 1e13 at six) is undefined behaviour to convert. No reading
    // on this dash is that big; one that is, is garbage, and reads as such.
    if (std::fabs(static_cast<double>(v)) * 1e6 >= 9.0e18) return emit_lit(buf, n, "--");
    char tmp[40];
    const size_t len = render_fixed(tmp, v, decimals);
    return emit(buf, n, tmp, len);
}

size_t fmt_value(char* buf, size_t n, float v, int decimals, bool valid) {
    if (!valid) return emit_lit(buf, n, "--");
    return fmt_float(buf, n, v, decimals);
}

size_t fmt_duration(char* buf, size_t n, uint32_t ms, bool valid) {
    if (!valid) return emit_lit(buf, n, "--:--.--");

    const uint32_t total_s = ms / 1000;
    const uint32_t h = total_s / 3600;
    const uint32_t m = (total_s / 60) % 60;
    const uint32_t s = total_s % 60;

    char tmp[24];
    size_t k = 0;
    if (h > 0) {
        // Over an hour, tenths are as much resolution as anyone reads.
        if (h >= 10) tmp[k++] = static_cast<char>('0' + (h / 10) % 10);
        tmp[k++] = static_cast<char>('0' + h % 10);
        tmp[k++] = ':';
        k += two_digits(tmp + k, m);
        tmp[k++] = ':';
        k += two_digits(tmp + k, s);
        tmp[k++] = '.';
        tmp[k++] = static_cast<char>('0' + (ms % 1000) / 100);
    } else {
        if (m >= 10) tmp[k++] = static_cast<char>('0' + (m / 10) % 10);
        tmp[k++] = static_cast<char>('0' + m % 10);   // no leading zero
        tmp[k++] = ':';
        k += two_digits(tmp + k, s);
        tmp[k++] = '.';
        k += two_digits(tmp + k, (ms % 1000) / 10);
    }
    return emit(buf, n, tmp, k);
}

size_t fmt_delta(char* buf, size_t n, int32_t ms, bool valid, int decimals) {
    if (!valid) return emit_lit(buf, n, "--");
    char tmp[24];
    size_t k = 0;
    // The sign is the whole point of a delta, so it is always drawn — a
    // rider glancing down reads the colour and the sign, not the digits.
    tmp[k++] = ms < 0 ? '-' : '+';
    // Widened before the negation: -INT32_MIN does not fit an int32_t.
    const int64_t mag = ms < 0 ? -static_cast<int64_t>(ms) : ms;
    const float secs = static_cast<float>(mag) / 1000.0f;
    k += render_fixed(tmp + k, secs, decimals);
    return emit(buf, n, tmp, k);
}

size_t fmt_clock(char* buf, size_t n, uint32_t utc_ms, int tz_offset_hours,
                 bool valid) {
    if (!valid) return emit_lit(buf, n, "--:--");

    constexpr int32_t kDay = 24 * 60 * 60;
    int32_t s = static_cast<int32_t>((utc_ms / 1000) % kDay);
    s += tz_offset_hours * 3600;
    s %= kDay;
    if (s < 0) s += kDay;

    char tmp[8];
    size_t k = 0;
    k += two_digits(tmp + k, static_cast<uint32_t>(s / 3600));
    tmp[k++] = ':';
    k += two_digits(tmp + k, static_cast<uint32_t>((s / 60) % 60));
    return emit(buf, n, tmp, k);
}

}  // namespace core::units
