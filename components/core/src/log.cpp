#include "core/log/log.hpp"
#include <cstdio>
#include <cstring>
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
    // Rounded once, to whole millionths, and split after: rounding only the
    // fraction wrote 39.9999996 as "39.000000", a point 69 miles off.
    const long long micro = static_cast<long long>(v * 1000000.0 + 0.5);
    const long long whole = micro / 1000000;
    if (neg && micro != 0) k = put(buf, n, k, '-');
    char tmp[24];
    size_t d = 0;
    long long w = whole;
    if (w == 0) tmp[d++] = '0';
    while (w > 0 && d < sizeof tmp) { tmp[d++] = static_cast<char>('0' + w % 10); w /= 10; }
    while (d > 0) k = put(buf, n, k, tmp[--d]);
    k = put(buf, n, k, '.');
    const long long f = micro % 1000000;
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

// One row per Rate, in enum order. The label and the interval live together
// because the only way they can lie is by being written apart.
struct RateRow { const char* label; uint32_t interval_ms; };

constexpr RateRow kRates[kRateCount] = {
    {"No log", 0},
    {"1 Hz",   1000},
    {"5 Hz",   200},
    {"10 Hz",  100},
    {"25 Hz",  40},
    {"50 Hz",  20},
};

// The logic tick is 20 ms and `now_ms` is a millisecond clock, so an interval
// measured between two ticks can read 19 as easily as 20. Without a little
// slack the 50 Hz option would defer every other tick and log at 25.
constexpr uint32_t kSlackMs = 2;

}  // namespace

// Derived from the table above rather than typed out a second time. Every
// initialiser is a constant expression, so this lands in flash and is ready
// before any static constructor runs — which matters, because the config
// item table in another translation unit points at it.
const char* const kRateLabels[kRateCount] = {
    kRates[0].label, kRates[1].label, kRates[2].label,
    kRates[3].label, kRates[4].label, kRates[5].label,
};

uint32_t rate_interval_ms(uint8_t rate) {
    // Out of range means an NVS blob from a firmware that knew more rates
    // than this one. Fall back to the default rather than to Off: a blackbox
    // that stopped recording because of a byte it did not recognise is the
    // worst of the available failures.
    if (rate >= kRateCount) rate = static_cast<uint8_t>(kDefaultRate);
    return kRates[rate].interval_ms;
}

bool Decimator::due(uint8_t rate, uint32_t now_ms, bool force) {
    const uint32_t interval = rate_interval_ms(rate);
    if (interval == 0) return false;                  // Off, forced or not
    if (primed_ && !force && now_ms - last_ms_ + kSlackMs < interval) {
        return false;
    }
    last_ms_ = now_ms;
    primed_  = true;
    return true;
}

size_t csv_header(char* buf, size_t n) {
    // The first line has to be exactly this for RaceRender to accept the
    // lap tags and the named columns. Anything else and it falls back to
    // guessing, which gets Time wrong.
    const char* h =
        "# RaceRender Data\n"
        "Time,Latitude,Longitude,MPH,RPM,X,Y,Lap,Heading,Gear,"
        "Lean,Pitch,GVert,LapTime,Sats,Flags,"
        "FrontMPH,RearMPH,GpsMPH,HDOP,GpsUtc,FusedMPH,FusedSigma,AltFt,HR,"
        "NavTurnFt,NavDestMi,LimitMPH,Volts,ShiftLamps,SlipPct\n";
    return terminate(buf, n, put_str(buf, n, 0, h));
}

// Two digits, zero padded. Lap tags are hh:mm:ss.nn and RaceRender parses
// them strictly.
namespace {
size_t put_2(char* buf, size_t n, size_t k, unsigned v) {
    k = put(buf, n, k, static_cast<char>('0' + (v / 10) % 10));
    return put(buf, n, k, static_cast<char>('0' + v % 10));
}
}  // namespace

size_t format_lap_marker(char* buf, size_t n, uint16_t index, uint32_t lap_ms) {
    size_t k = put_str(buf, n, 0, "# Lap ");
    k = put_num(buf, n, k, static_cast<float>(index), 0);
    k = put_str(buf, n, k, ": ");
    const uint32_t total_s = lap_ms / 1000u;
    k = put_2(buf, n, k, total_s / 3600u);
    k = put(buf, n, k, ':');
    k = put_2(buf, n, k, (total_s / 60u) % 60u);
    k = put(buf, n, k, ':');
    k = put_2(buf, n, k, total_s % 60u);
    k = put(buf, n, k, '.');
    // Hundredths, which is the resolution the tag format carries.
    k = put_2(buf, n, k, (lap_ms % 1000u) / 10u);
    k = put(buf, n, k, '\n');
    return terminate(buf, n, k);
}

size_t format_csv(char* buf, size_t n, const Record& r) {
    size_t k = 0;
    // Seconds, not milliseconds. This is the one column RaceRender requires
    // and the one it would silently misread.
    k = put_num(buf, n, k, static_cast<float>(r.t_ms) / 1000.0f, 3);
    k = put(buf, n, k, ',');
    k = put_deg(buf, n, k, r.lat);
    k = put(buf, n, k, ',');
    k = put_deg(buf, n, k, r.lon);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.speed_mph, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.rpm, 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.g_lon, 2);          // X
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.g_lat, 2);          // Y
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.lap_num), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.course_deg, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.gear), 0);
    k = put(buf, n, k, ',');
    // Ours from here on. RaceRender ignores what it does not know, and
    // these are what the blackbox is actually for.
    k = put_num(buf, n, k, r.lean_deg, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.pitch_deg, 1);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.g_vert, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.lap_ms) / 1000.0f, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.sats), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.flags), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.front_mph, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.rear_mph, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.gps_mph, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.hdop, 1);
    k = put(buf, n, k, ',');
    // Seconds of the UTC day. A float holds them to a few ms, which is all
    // the replay needs: it only asks whether this differs from the last row.
    k = put_num(buf, n, k, static_cast<float>(r.gps_utc_ms) / 1000.0f, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.fused_mph, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.fused_sigma, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.alt_ft, 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.hr_bpm), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.nav_turn_ft, 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.nav_dest_mi, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.limit_mph), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, r.vbat, 2);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.shift_lamps), 0);
    k = put(buf, n, k, ',');
    k = put_num(buf, n, k, static_cast<float>(r.slip_pct), 0);
    k = put(buf, n, k, '\n');
    return terminate(buf, n, k);
}

namespace {
bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in(int y, int m) {
    static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return kDays[m - 1] + (m == 2 && leap(y) ? 1 : 0);
}
}  // namespace

Date local_date(Date utc, uint32_t utc_ms, int32_t tz_h) {
    if (utc.year == 0 || utc.month < 1 || utc.month > 12 || utc.day < 1) return utc;
    const int64_t local_ms = static_cast<int64_t>(utc_ms) + static_cast<int64_t>(tz_h) * 3600000;
    int y = utc.year, m = utc.month, d = utc.day;
    if (local_ms < 0) {
        if (--d < 1) {
            if (--m < 1) { m = 12; --y; }
            d = days_in(y, m);
        }
    } else if (local_ms >= 86400000) {
        if (++d > days_in(y, m)) {
            d = 1;
            if (++m > 12) { m = 1; ++y; }
        }
    }
    Date out;
    out.year = static_cast<uint16_t>(y);
    out.month = static_cast<uint8_t>(m);
    out.day = static_cast<uint8_t>(d);
    return out;
}

size_t log_file_name(char* buf, size_t n, Date date, unsigned session) {
    if (!buf || n == 0) return 0;
    session %= 1000;
    int len;
    if (date.year == 0) {
        len = std::snprintf(buf, n, "logs/s%03u.csv", session);
    } else {
        len = std::snprintf(buf, n, "logs/%04u-%02u-%02u_s%03u.csv",
                            static_cast<unsigned>(date.year),
                            static_cast<unsigned>(date.month),
                            static_cast<unsigned>(date.day), session);
    }
    if (len < 0 || static_cast<size_t>(len) >= n) { buf[0] = '\0'; return 0; }
    return static_cast<size_t>(len);
}

// session_of() is in detail.cpp: it knows the detail files' names too.

void FlushPolicy::buffered(uint32_t now_ms) {
    if (!has_rows_) {
        has_rows_ = true;
        oldest_ms_ = now_ms;
    }
}

void FlushPolicy::written(uint32_t now_ms) {
    has_rows_ = false;
    if (!unsynced_) {
        unsynced_ = true;
        unsynced_since_ms_ = now_ms;
    }
}

void FlushPolicy::synced() {
    unsynced_ = false;
}

Flush FlushPolicy::decide(uint32_t now_ms, bool stopped, const FlushConfig& c) {
    const bool stop_edge = stopped && !was_stopped_;
    was_stopped_ = stopped;
    const bool pending = has_rows_ || unsynced_;
    if (stop_edge && pending) return Flush::WriteAndSync;
    // Unsigned differences, so a wrapped millisecond counter still counts.
    if (unsynced_ && now_ms - unsynced_since_ms_ >= c.sync_every_ms) {
        return Flush::WriteAndSync;
    }
    if (has_rows_ && now_ms - oldest_ms_ >= c.max_age_ms) return Flush::Write;
    return Flush::None;
}

}  // namespace core::log
