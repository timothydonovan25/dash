#include <cmath>
#include "check.hpp"
#include "core/geo/geo.hpp"
using namespace core::geo;

int main() {
    CASE("distance against a known pair");
    // Statue of Liberty to Eiffel Tower, ~5837 km = ~19.15 million ft.
    LatLon liberty{40.6892, -74.0445};
    LatLon eiffel {48.8584,   2.2945};
    CHECK_NEAR(distance_ft(liberty, eiffel) / 5280.0, 3627.0, 15.0);  // miles

    CASE("distance is zero to itself and symmetric");
    CHECK_NEAR(distance_ft(liberty, liberty), 0.0, 1e-6);
    CHECK_NEAR(distance_ft(liberty, eiffel), distance_ft(eiffel, liberty), 1.0);

    CASE("one degree of latitude is about 364000 ft");
    CHECK_NEAR(distance_ft({40.0, -90.0}, {41.0, -90.0}), 364000.0, 900.0);

    CASE("bearings on the cardinals");
    CHECK_NEAR(bearing_deg({40.0, -90.0}, {41.0, -90.0}),   0.0, 0.01);  // N
    CHECK_NEAR(bearing_deg({40.0, -90.0}, {40.0, -89.0}),  90.0, 0.50);  // E
    CHECK_NEAR(bearing_deg({40.0, -90.0}, {39.0, -90.0}), 180.0, 0.01);  // S
    CHECK_NEAR(bearing_deg({40.0, -90.0}, {40.0, -91.0}), 270.0, 0.50);  // W

    CASE("heading delta takes the short way round");
    CHECK_NEAR(heading_delta_deg(350.0,  10.0),  20.0, 1e-9);
    CHECK_NEAR(heading_delta_deg( 10.0, 350.0), -20.0, 1e-9);
    // Exactly 180 is ambiguous; the convention here is the negative side.
    CHECK_NEAR(heading_delta_deg(  0.0, 180.0), -180.0, 1e-9);

    // Finish line at the origin point, crossed heading due north,
    // 50 ft either side of centre.
    LatLon line{40.000000, -90.000000};
    const double north = 0.0;
    const double half  = 50.0;
    const double ft_lat = 1.0 / 364000.0;                 // degrees per foot
    const double ft_lon = 1.0 / (364000.0 * 0.766);       // cos(40 deg)

    CASE("northbound crossing is detected");
    LatLon a{line.lat - 20 * ft_lat, line.lon};
    LatLon b{line.lat + 20 * ft_lat, line.lon};
    Crossing x = crosses_line(a, b, line, north, half);
    CHECK(x.crossed);
    CHECK_NEAR(x.fraction, 0.5, 0.02);
    CHECK_NEAR(x.lateral_ft, 0.0, 0.5);

    CASE("fraction interpolates where in the segment the line was cut");
    LatLon c{line.lat - 5  * ft_lat, line.lon};
    LatLon d{line.lat + 45 * ft_lat, line.lon};
    x = crosses_line(c, d, line, north, half);
    CHECK(x.crossed);
    CHECK_NEAR(x.fraction, 0.1, 0.02);

    CASE("southbound does NOT trigger — direction matters");
    x = crosses_line(b, a, line, north, half);
    CHECK(!x.crossed);

    CASE("riding past the line outside its width does not trigger");
    LatLon e{line.lat - 20 * ft_lat, line.lon + 80 * ft_lon};
    LatLon f{line.lat + 20 * ft_lat, line.lon + 80 * ft_lon};
    x = crosses_line(e, f, line, north, half);
    CHECK(!x.crossed);          // 80 ft off centre, line is 50 ft half-width

    CASE("just inside the width does trigger");
    LatLon g{line.lat - 20 * ft_lat, line.lon + 45 * ft_lon};
    LatLon h{line.lat + 20 * ft_lat, line.lon + 45 * ft_lon};
    x = crosses_line(g, h, line, north, half);
    CHECK(x.crossed);
    CHECK_NEAR(x.lateral_ft, 45.0, 3.0);

    CASE("approaching but stopping short does not trigger");
    LatLon i{line.lat - 40 * ft_lat, line.lon};
    LatLon j{line.lat - 10 * ft_lat, line.lon};
    CHECK(!crosses_line(i, j, line, north, half).crossed);

    CASE("sitting still on the line does not fire repeatedly");
    LatLon k{line.lat + 1 * ft_lat, line.lon};
    for (int n = 0; n < 20; ++n) CHECK(!crosses_line(k, k, line, north, half).crossed);

    CASE("travel parallel to the line never counts as a crossing");
    // Line runs east-west, rider travels north-south along it. The
    // along-track component is floating-point noise, not motion.
    x = crosses_line(a, b, line, 90.0, half);
    CHECK(!x.crossed);
    for (int n = -30; n <= 30; ++n) {
        LatLon u{line.lat + n * ft_lat, line.lon};
        LatLon v{line.lat + (n + 1) * ft_lat, line.lon};
        CHECK(!crosses_line(u, v, line, 90.0, half).crossed);
    }

    CASE("sub-foot jitter on the line does not score laps");
    for (int n = 0; n < 200; ++n) {
        const double j = ((n * 37) % 11 - 5) * 0.04;   // +/- 0.2 ft
        LatLon u{line.lat + j * ft_lat, line.lon};
        LatLon v{line.lat - j * ft_lat, line.lon};
        CHECK(!crosses_line(u, v, line, north, half).crossed);
    }

    CASE("CONTRACT: real GPS jitter DOES cross — the caller must gate on speed");
    // A stationary consumer receiver wanders several feet, which is real
    // northbound motion as far as the geometry is concerned. Raising the
    // guard high enough to swallow it would also swallow a genuine slow
    // crossing (3 mph at 10 Hz is 0.44 ft per fix). So this function does
    // NOT try. The lap timer owns jitter rejection, via a minimum-speed
    // arm threshold and a minimum lap gap. This test exists to pin that
    // contract down so nobody "fixes" it here later.
    LatLon w{line.lat - 4.0 * ft_lat, line.lon};
    LatLon y{line.lat + 4.0 * ft_lat, line.lon};
    CHECK(crosses_line(w, y, line, north, half).crossed);

    CASE("a 60 mph fix pair at 10 Hz still resolves the crossing");
    // 60 mph = 88 ft/s, so 8.8 ft between fixes.
    LatLon p{line.lat - 3.0 * ft_lat, line.lon};
    LatLon q{line.lat + 5.8 * ft_lat, line.lon};
    x = crosses_line(p, q, line, north, half);
    CHECK(x.crossed);
    CHECK_NEAR(x.fraction, 3.0 / 8.8, 0.03);   // worth ~65 ms of lap accuracy

    CASE("guide: heading-up when riding, north-up when parked");
    {
        const LatLon here{40.0, -75.0};
        const LatLon north{40.0 + 1000.0 / 364000.0, -75.0};   // ~1000 ft north
        // Riding east: the waypoint is 90 degrees to the left.
        Guide g = guide(here, 90.0f, 20.0f, north);
        CHECK(g.heading_up);
        CHECK_NEAR(g.dist_ft, 1000.0, 15.0);
        CHECK_NEAR(g.bearing_deg, 0.0, 0.5);
        CHECK_NEAR(g.rel_deg, -90.0, 0.5);
        // Riding south: straight behind, which is +/-180.
        g = guide(here, 180.0f, 20.0f, north);
        CHECK_NEAR(std::fabs(g.rel_deg), 180.0, 0.5);
        // Parked, the course is noise: measured from north instead.
        g = guide(here, 90.0f, 1.0f, north);
        CHECK(!g.heading_up);
        CHECK_NEAR(g.rel_deg, 0.0, 0.5);
        // On top of it.
        g = guide(here, 0.0f, 0.0f, here);
        CHECK_NEAR(g.dist_ft, 0.0, 0.01);
    }

    return check::report("geo");
}
