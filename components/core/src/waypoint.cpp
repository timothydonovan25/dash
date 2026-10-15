#include "core/waypoint/waypoint.hpp"

namespace core::waypoint {

namespace {
constexpr double kDayMs = 86400000.0;
}

const char* name(size_t i) {
    static const char* const kNames[kCount] = {"START", "TRUCK", "CAMP", "FUEL", "MARK"};
    return i < kCount ? kNames[i] : "?";
}

uint32_t gps_day(uint16_t year, uint16_t day_of_year) {
    if (year == 0 || day_of_year == 0) return 0;
    return static_cast<uint32_t>(year) * 366u + day_of_year;
}

bool start_is_stale(const state::Waypoint& start, uint32_t day, uint32_t utc_ms,
                    uint32_t stale_ms) {
    if (!start.set) return true;
    // Without both dates there is no telling how old it is; keep it rather
    // than throw away a way home on a guess.
    if (day == 0 || start.day == 0) return false;
    const double then = start.day * kDayMs + start.utc_ms;
    const double now = day * kDayMs + utc_ms;
    // A day apart in gps_day terms is a real day apart only within a year,
    // but across New Year the numbers jump by about a year, which is stale
    // either way.
    return now - then > static_cast<double>(stale_ms) || now < then;
}

bool good_fix(const state::Gnss& g) {
    return g.fix && g.hdop > 0.0f && g.hdop <= kMaxHdop;
}

double encode_when(const state::Waypoint& w) {
    if (!w.set) return -1.0;
    return w.day * kDayMs + w.utc_ms;
}

void decode(double lat, double lon, double when, state::Waypoint& out) {
    out = state::Waypoint{};
    if (!(when >= 0.0)) return;                 // cleared, or NaN
    out.set = true;
    out.lat = lat;
    out.lon = lon;
    const double day = static_cast<double>(static_cast<uint64_t>(when / kDayMs));
    out.day = static_cast<uint32_t>(day);
    out.utc_ms = static_cast<uint32_t>(when - day * kDayMs);
}

}  // namespace core::waypoint
