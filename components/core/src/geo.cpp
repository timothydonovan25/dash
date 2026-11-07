#include "core/geo/geo.hpp"
#include <cmath>

namespace core::geo {
namespace {
constexpr double kPi        = 3.14159265358979323846;
constexpr double kDeg2Rad   = kPi / 180.0;
constexpr double kRad2Deg   = 180.0 / kPi;
constexpr double kEarthRadiusFt = 20902231.0;   // mean radius

inline double rad(double d) { return d * kDeg2Rad; }
}  // namespace

double distance_ft(LatLon a, LatLon b) {
    const double dlat = rad(b.lat - a.lat);
    const double dlon = rad(b.lon - a.lon);
    const double la = rad(a.lat), lb = rad(b.lat);
    const double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(la) * std::cos(lb) *
                     std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * kEarthRadiusFt * std::asin(std::sqrt(h < 1.0 ? h : 1.0));
}

double bearing_deg(LatLon from, LatLon to) {
    const double la = rad(from.lat), lb = rad(to.lat);
    const double dlon = rad(to.lon - from.lon);
    const double y = std::sin(dlon) * std::cos(lb);
    const double x = std::cos(la) * std::sin(lb) -
                     std::sin(la) * std::cos(lb) * std::cos(dlon);
    double d = std::atan2(y, x) * kRad2Deg;
    if (d < 0.0) d += 360.0;
    return d;
}

double heading_delta_deg(double from_deg, double to_deg) {
    double d = std::fmod(to_deg - from_deg + 540.0, 360.0) - 180.0;
    return d;
}

Crossing crosses_line(LatLon prev, LatLon cur, LatLon line,
                      double line_heading_deg, double half_width_ft) {
    Crossing out;

    // Flat local frame centred on the line point. Over the few hundred
    // feet that matter here the error is far below GPS noise.
    const double lat_ft_per_deg = 364000.0;
    const double lon_ft_per_deg = 364000.0 * std::cos(rad(line.lat));

    const double px = (prev.lon - line.lon) * lon_ft_per_deg;
    const double py = (prev.lat - line.lat) * lat_ft_per_deg;
    const double cx = (cur.lon  - line.lon) * lon_ft_per_deg;
    const double cy = (cur.lat  - line.lat) * lat_ft_per_deg;

    // Unit vector along the direction of travel across the line.
    const double h  = rad(line_heading_deg);
    const double ux = std::sin(h), uy = std::cos(h);
    // Perpendicular, i.e. along the line itself.
    const double vx = uy, vy = -ux;

    const double p_along = px * ux + py * uy;
    const double c_along = cx * ux + cy * uy;

    // Must go from behind the line to in front of it, in that order.
    if (!(p_along < 0.0 && c_along >= 0.0)) return out;

    const double denom = c_along - p_along;

    // Reject motion that is parallel to the line. Without this, a rider
    // travelling exactly along the line produces an along-track component
    // of ~1e-15 whose sign flip is pure floating-point noise, and every
    // GPS fix scores a lap. Same guard rejects a stationary bike jittering
    // on the line, which is how you get phantom laps in the paddock.
    constexpr double kMinAlongFt = 0.5;
    if (denom < kMinAlongFt) return out;

    const double t = -p_along / denom;

    const double ix = px + (cx - px) * t;
    const double iy = py + (cy - py) * t;
    const double lateral = ix * vx + iy * vy;

    if (std::fabs(lateral) > half_width_ft) return out;

    out.crossed = true;
    out.fraction = t;
    out.lateral_ft = lateral;
    return out;
}

}  // namespace core::geo
