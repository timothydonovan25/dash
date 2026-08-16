#include "core/log/log.hpp"
#include "core/units/units.hpp"

namespace core::log {
namespace {

size_t put(char* buf, size_t n, size_t k, char c) {
    if (k + 1 < n) buf[k] = c;
    return k + 1;
}

size_t put_str(char* buf, size_t n, size_t k, const char* s) {
    for (; *s; ++s) k = put(buf, n, k, *s);
    return k;
}

// Latitude and longitude need more than a float's seven digits: 1e-5 of a
// degree is about three feet, and a trace quantised to thirty would not show
// which side of the track you took.
size_t put_deg(char* buf, size_t n, size_t k, double v) {
    if (k >= n) return k;
    const bool neg = v < 0.0;
    if (neg) v = -v;
    const long long whole = static_cast<long long>(v);
    const double frac = v - static_cast<double>(whole);
    if (neg) k = put(buf, n, k, '-');
    char tmp[24];
    size_t d = 0;
    long long w = whole;
    if (w == 0) tmp[d++] = '0';
    while (w > 0 && d < sizeof tmp) { tmp[d++] = static_cast<char>('0' + w % 10); w /= 10; }
    while (d > 0) k = put(buf, n, k, tmp[--d]);
    k = put(buf, n, k, '.');
    long long f = static_cast<long long>(frac * 1000000.0 + 0.5);
    for (int i = 5; i >= 0; --i) {
        long long div = 1;
        for (int j = 0; j < i; ++j) div *= 10;
        k = put(buf, n, k, static_cast<char>('0' + (f / div) % 10));
    }
    return k;
}

size_t put_num(char* buf, size_t n, size_t k, float v, int decimals) {
    char tmp[24];
    const size_t len = units::fmt_float(tmp, sizeof tmp, v, decimals);
    for (size_t i = 0; i < len; ++i) k = put(buf, n, k, tmp[i]);
    return k;
}

size_t terminate(char* buf, size_t n, size_t k) {
    if (n == 0) return 0;
    const size_t w = k < n - 1 ? k : n - 1;
    buf[w] = '\0';
    return w;
}

}  // namespace

size_t csv_header(char* buf, size_t n) {
    const char* h =
        "t_ms,speed_mph,rpm,lean_deg,pitch_deg,g_lat,g_vert,lat,lon,"
        "lap_ms,sats,flags\n";
    return terminate(buf, n, put_str(buf, n, 0, h));
}

size_t format_csv(char* buf, size_t n, const Record& r) {
    size_t k = 0;
    k = put_num(buf, n, k, static_cast<float>(r.t_ms), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.speed_mph, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.rpm, 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.lean_deg, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.pitch_deg, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.g_lat, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.g_vert, 2);
    k = put(buf, n, k, ',');
    k = put_deg(buf, n, k, r.lat);
    k = put(buf, n, k, ',');
    k = put_deg(buf, n, k, r.lon);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.lap_ms), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.sats), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.flags), 0);
    k = put(buf, n, k, '\n');
    return terminate(buf, n, k);
}

}  // namespace core::log
