#include "core/track/track.hpp"
#include <cstdio>
#include <cstring>

namespace core::track {
namespace {

// Byte layout of the header. Offsets, not a packed struct: the compiler
// gets no say in where a field lands, and the file outlives any one build.
constexpr size_t kOffMagic   = 0;
constexpr size_t kOffVersion = 4;
constexpr size_t kOffPoints  = 6;
constexpr size_t kOffUsed    = 8;
constexpr size_t kOffSplits  = 9;
constexpr size_t kOffSess    = 10;
constexpr size_t kOffFinish  = 12;     // lat, lon, heading: three f64
constexpr size_t kOffSplit0  = 36;
constexpr size_t kLineBytes  = 24;
constexpr size_t kOffBest    = kOffSplit0 + (lap::kSectors - 1) * kLineBytes;
constexpr size_t kOffSector  = kOffBest + 4;
constexpr size_t kOffDay     = kOffSector + 4 * lap::kSectors;
constexpr size_t kOffRef     = kOffDay + 4;
constexpr size_t kOffHeadCrc = kHeaderBytes - 4;
static_assert(kOffRef + 4 <= kOffHeadCrc, "the header has outgrown its 128 bytes");

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
void put_f64(uint8_t* p, double d) {
    uint64_t v;
    std::memcpy(&v, &d, sizeof v);
    put_u64(p, v);
}
double get_f64(const uint8_t* p) {
    const uint64_t v = get_u64(p);
    double d;
    std::memcpy(&d, &v, sizeof d);
    return d;
}
void put_f32(uint8_t* p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof v);
    put_u32(p, v);
}
float get_f32(const uint8_t* p) {
    const uint32_t v = get_u32(p);
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

void put_line(uint8_t* p, const Line& l) {
    put_f64(p, l.at.lat);
    put_f64(p + 8, l.at.lon);
    put_f64(p + 16, l.heading_deg);
}
Line get_line(const uint8_t* p) {
    Line l;
    l.at.lat = get_f64(p);
    l.at.lon = get_f64(p + 8);
    l.heading_deg = get_f64(p + 16);
    return l;
}

}  // namespace

uint32_t Record::ideal_ms() const {
    uint32_t sum = 0;
    for (uint32_t s : sector_best_ms) {
        if (s == 0) return 0;
        sum += s;
    }
    return sum;
}

int nearest(const Library& lib, geo::LatLon pos, double within_ft) {
    int best = -1;
    double best_ft = within_ft;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!lib.rec[i].used) continue;
        const double d = geo::distance_ft(pos, lib.rec[i].finish.at);
        if (d <= best_ft) {
            best_ft = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int slot_for_line(const Library& lib, geo::LatLon at) {
    const int same = nearest(lib, at, kSameTrackFt);
    if (same >= 0) return same;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!lib.rec[i].used) return static_cast<int>(i);
    }
    // Full: the one ridden longest ago. Equal days (two never dated) go to
    // the lower slot, so the choice is the same every time.
    size_t oldest = 0;
    for (size_t i = 1; i < kMaxTracks; ++i) {
        if (lib.rec[i].last_day < lib.rec[oldest].last_day) oldest = i;
    }
    return static_cast<int>(oldest);
}

Record fresh(const Line& finish, uint32_t day) {
    Record r;
    r.used = true;
    r.finish = finish;
    r.last_day = day;
    r.sessions = 1;
    return r;
}

bool offer_lap(Record& r, uint32_t lap_ms, bool clean) {
    if (!clean || lap_ms == 0) return false;
    if (r.best_ms != 0 && lap_ms >= r.best_ms) return false;
    r.best_ms = lap_ms;
    return true;
}

bool offer_sectors(Record& r, const uint32_t (&session_best)[lap::kSectors]) {
    bool changed = false;
    for (int k = 0; k < lap::kSectors; ++k) {
        const uint32_t s = session_best[k];
        if (s == 0) continue;
        if (r.sector_best_ms[k] == 0 || s < r.sector_best_ms[k]) {
            r.sector_best_ms[k] = s;
            changed = true;
        }
    }
    return changed;
}

void shift(delta::Point* pts, size_t n, float dx_ft, float dy_ft) {
    for (size_t i = 0; i < n; ++i) {
        pts[i].x_ft += dx_ft;
        pts[i].y_ft += dy_ft;
    }
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    // Bitwise, no table: a track file is written a few times a session, and
    // 1 kB of table in flash buys nothing here.
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t encode(const Record& r, const delta::Point* pts, size_t n,
              uint8_t* out, size_t cap) {
    if (n > delta::kMaxPoints || (n > 0 && !pts)) return 0;
    const size_t total = kHeaderBytes + n * kPointBytes + 4;
    if (!out || cap < total) return 0;

    std::memset(out, 0, kHeaderBytes);
    put_u32(out + kOffMagic, kMagic);
    put_u16(out + kOffVersion, kVersion);
    put_u16(out + kOffPoints, static_cast<uint16_t>(n));
    out[kOffUsed] = r.used ? 1 : 0;
    out[kOffSplits] = r.splits ? 1 : 0;
    put_u16(out + kOffSess, r.sessions);
    put_line(out + kOffFinish, r.finish);
    for (int k = 0; k < lap::kSectors - 1; ++k) {
        put_line(out + kOffSplit0 + k * kLineBytes, r.split[k]);
    }
    put_u32(out + kOffBest, r.best_ms);
    for (int k = 0; k < lap::kSectors; ++k) {
        put_u32(out + kOffSector + 4 * k, r.sector_best_ms[k]);
    }
    put_u32(out + kOffDay, r.last_day);
    put_u32(out + kOffRef, n > 0 ? r.ref_ms : 0);
    put_u32(out + kOffHeadCrc, crc32(out, kOffHeadCrc));

    uint8_t* p = out + kHeaderBytes;
    for (size_t i = 0; i < n; ++i, p += kPointBytes) {
        put_f32(p, pts[i].x_ft);
        put_f32(p + 4, pts[i].y_ft);
        put_u32(p + 8, pts[i].elapsed_ms);
    }
    put_u32(p, crc32(out + kHeaderBytes, n * kPointBytes));
    return total;
}

bool decode_header(const uint8_t* buf, size_t len, Record& r, size_t& n_points) {
    n_points = 0;
    if (!buf || len < kHeaderBytes) return false;
    if (get_u32(buf + kOffMagic) != kMagic) return false;
    if (get_u16(buf + kOffVersion) != kVersion) return false;
    if (get_u32(buf + kOffHeadCrc) != crc32(buf, kOffHeadCrc)) return false;

    Record out;
    out.used = buf[kOffUsed] != 0;
    out.splits = buf[kOffSplits] != 0;
    out.sessions = get_u16(buf + kOffSess);
    out.finish = get_line(buf + kOffFinish);
    for (int k = 0; k < lap::kSectors - 1; ++k) {
        out.split[k] = get_line(buf + kOffSplit0 + k * kLineBytes);
    }
    out.best_ms = get_u32(buf + kOffBest);
    for (int k = 0; k < lap::kSectors; ++k) {
        out.sector_best_ms[k] = get_u32(buf + kOffSector + 4 * k);
    }
    out.last_day = get_u32(buf + kOffDay);
    out.ref_ms = get_u32(buf + kOffRef);
    r = out;
    n_points = get_u16(buf + kOffPoints);
    return true;
}

bool decode(const uint8_t* buf, size_t len, Record& r, delta::Point* pts,
            size_t cap, size_t& n) {
    n = 0;
    size_t want = 0;
    if (!decode_header(buf, len, r, want)) return false;
    // From here on the track itself is good; only the reference may not be.
    if (want == 0 || want > cap || want > delta::kMaxPoints || !pts) return true;
    const size_t bytes = want * kPointBytes;
    if (len < kHeaderBytes + bytes + 4) return true;
    const uint8_t* p = buf + kHeaderBytes;
    if (get_u32(p + bytes) != crc32(p, bytes)) return true;
    for (size_t i = 0; i < want; ++i, p += kPointBytes) {
        pts[i].x_ft = get_f32(p);
        pts[i].y_ft = get_f32(p + 4);
        pts[i].elapsed_ms = get_u32(p + 8);
    }
    n = want;
    return true;
}

bool file_name(char* out, size_t cap, size_t slot, bool temp) {
    if (!out || slot >= kMaxTracks) return false;
    const int w = std::snprintf(out, cap, "%s/t%02u.%s", kDir,
                                static_cast<unsigned>(slot), temp ? "tmp" : "trk");
    return w > 0 && static_cast<size_t>(w) < cap;
}

}  // namespace core::track
