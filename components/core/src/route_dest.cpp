#include "core/route/dest.hpp"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "core/route/route.hpp"

namespace core::route {

namespace {

bool parse_deg(const char* s, size_t n, double lo, double hi, int32_t& out) {
    char buf[24];
    while (n && (*s == ' ' || *s == '\t')) { ++s; --n; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) --n;
    if (n == 0 || n >= sizeof buf) return false;
    std::memcpy(buf, s, n);
    buf[n] = '\0';
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end != buf + n || !(v >= lo && v <= hi)) return false;
    out = int32_t(std::lround(v * 1e7));
    return true;
}

}  // namespace

bool parse_dest_line(const char* line, size_t len, Dest& out) {
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
    if (len == 0 || line[0] == '#') return false;
    const char* c1 = static_cast<const char*>(std::memchr(line, ',', len));
    if (!c1) return false;
    const char* rest = c1 + 1;
    const size_t rest_n = len - size_t(rest - line);
    const char* c2 = static_cast<const char*>(std::memchr(rest, ',', rest_n));
    if (!c2) return false;
    Dest d;
    if (!parse_deg(rest, size_t(c2 - rest), -90, 90, d.p.lat_e7)) return false;
    if (!parse_deg(c2 + 1, len - size_t(c2 + 1 - line), -180, 180, d.p.lon_e7)) return false;
    size_t n = size_t(c1 - line);
    while (n && line[n - 1] == ' ') --n;
    if (n == 0) return false;
    if (n >= kDestNameLen) n = kDestNameLen - 1;
    std::memcpy(d.name, line, n);
    d.name[n] = '\0';
    out = d;
    return true;
}

bool parse_go(const char* line, size_t len, Dest& out) {
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
    if (len < 2 || line[0] != 'G' || line[1] != ' ') return false;
    // Two numbers, each up to the next space, then the name.
    size_t i = 2;
    int32_t v[2];
    const double lim[2] = {90, 180};
    for (int k = 0; k < 2; ++k) {
        while (i < len && line[i] == ' ') ++i;
        const size_t start = i;
        while (i < len && line[i] != ' ') ++i;
        if (!parse_deg(line + start, i - start, -lim[k], lim[k], v[k])) return false;
    }
    while (i < len && line[i] == ' ') ++i;
    Dest d;
    d.p = Point{v[0], v[1]};
    size_t n = len - i;
    if (n >= kDestNameLen) n = kDestNameLen - 1;
    if (n) std::memcpy(d.name, line + i, n);
    else std::strcpy(d.name, "PHONE");
    d.name[n ? n : 5] = '\0';
    out = d;
    return true;
}

size_t format_dest_line(const Dest& d, char* out, size_t cap) {
    char name[kDestNameLen];
    size_t n = 0;
    for (const char* c = d.name; *c && n + 1 < sizeof name; ++c)
        name[n++] = *c == ',' ? ' ' : *c;      // a comma would split the line
    name[n] = '\0';
    const auto deg = [](int32_t e7, char* buf, size_t b) {
        const uint32_t a = uint32_t(e7 < 0 ? -int64_t(e7) : e7);
        return std::snprintf(buf, b, "%s%u.%07u", e7 < 0 ? "-" : "", unsigned(a / 10000000u), unsigned(a % 10000000u));
    };
    char lat[16], lon[16];
    deg(d.p.lat_e7, lat, sizeof lat);
    deg(d.p.lon_e7, lon, sizeof lon);
    const int k = std::snprintf(out, cap, "%s,%s,%s\n", name[0] ? name : "Place", lat, lon);
    return k > 0 && size_t(k) < cap ? size_t(k) : 0;
}

size_t push_recent(const Dest& fresh, const Dest* old, size_t n_old, Dest* out, size_t max) {
    if (max == 0) return 0;
    size_t n = 0;
    out[n] = fresh;
    out[n++].kind = kDestRecent;
    for (size_t i = 0; i < n_old && n < max; ++i) {
        // Within 50 m is the same place, whatever it was called.
        if (dist_m(old[i].p, fresh.p) < 50.0) continue;
        out[n] = old[i];
        out[n++].kind = kDestRecent;
    }
    return n;
}

size_t parse_dests(const char* text, size_t len, Dest* out, size_t max) {
    size_t n = 0, start = 0;
    for (size_t i = 0; i <= len && n < max; ++i) {
        if (i == len || text[i] == '\n') {
            if (parse_dest_line(text + start, i - start, out[n])) ++n;
            start = i + 1;
        }
    }
    return n;
}

size_t nearest_pois(Graph& g, Point here, uint8_t kind, Dest* out, size_t max) {
    if (!g.is_open() || max == 0) return 0;
    size_t n = 0;
    double dist[8];
    if (max > 8) max = 8;
    for (uint32_t i = 0; i < g.header().poi_count; ++i) {
        Poi p;
        if (!g.poi(i, p)) return 0;
        if (p.kind != kind) continue;
        const double d = dist_m(here, Point{p.lat_e7, p.lon_e7});
        if (n == max && d >= dist[n - 1]) continue;
        size_t k = n < max ? n++ : n - 1;          // insertion sort, nearest first
        while (k > 0 && dist[k - 1] > d) {
            dist[k] = dist[k - 1];
            out[k] = out[k - 1];
            --k;
        }
        dist[k] = d;
        Dest& o = out[k];
        o = Dest{};
        o.p = Point{p.lat_e7, p.lon_e7};
        o.kind = kind;
        g.name(p.name_off, o.name, sizeof o.name);
        if (!o.name[0]) std::strcpy(o.name, kind == PoiFuel ? "Fuel" : kind == PoiFood ? "Food" : "Parking");
    }
    return n;
}

}  // namespace core::route
