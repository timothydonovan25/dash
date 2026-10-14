#include "core/sun/sun.hpp"
#include <cmath>

namespace core::sun {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
}  // namespace

float elevation_deg(uint16_t day_of_year, uint32_t utc_ms,
                    double lat_deg, double lon_deg) {
    const double doy = static_cast<double>(day_of_year);
    const double utc_h = static_cast<double>(utc_ms % 86400000u) / 3600000.0;

    // Declination, and the equation of time in minutes (the sun is up to a
    // quarter of an hour fast or slow of the clock through the year).
    const double decl = -23.44 * std::cos(360.0 / 365.0 * (doy + 10.0) * kRad);
    const double b = 360.0 / 364.0 * (doy - 81.0) * kRad;
    const double eot_min = 9.87 * std::sin(2.0 * b) - 7.53 * std::cos(b) -
                           1.5 * std::sin(b);

    const double solar_h = utc_h + lon_deg / 15.0 + eot_min / 60.0;
    const double hour_angle = (solar_h - 12.0) * 15.0 * kRad;

    const double lat = lat_deg * kRad;
    const double d = decl * kRad;
    double s = std::sin(lat) * std::sin(d) +
               std::cos(lat) * std::cos(d) * std::cos(hour_angle);
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    return static_cast<float>(std::asin(s) / kRad);
}

uint8_t dim_pct(float elev, const DimCurve& c) {
    if (elev >= c.day_deg) return 100;
    if (elev <= c.night_deg) return c.night_pct;
    const float t = (elev - c.night_deg) / (c.day_deg - c.night_deg);   // 0..1
    return static_cast<uint8_t>(c.night_pct + t * (100.0f - c.night_pct) + 0.5f);
}

uint8_t apply_dim(uint8_t setting_pct, uint8_t dim) {
    if (setting_pct == 0) return 0;
    const uint32_t v = (static_cast<uint32_t>(setting_pct) * dim + 50) / 100;
    return static_cast<uint8_t>(v < 1 ? 1 : v);
}

uint8_t AutoDim::update(bool have_time, uint16_t day_of_year, uint32_t utc_ms,
                        double lat_deg, double lon_deg, bool enabled) {
    if (!enabled) {
        pct_ = 100;
    } else if (have_time && day_of_year != 0) {
        pct_ = dim_pct(elevation_deg(day_of_year, utc_ms, lat_deg, lon_deg));
    }
    return pct_;
}

}  // namespace core::sun
