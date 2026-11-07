// Geodesy for the lap timer and the map screens. Pure logic. (Routing has
// its own, on the map's integer coordinates: core/route/route.hpp.)
#pragma once

namespace core::geo {

struct LatLon {
    double lat = 0.0;
    double lon = 0.0;
};

// Great-circle distance. Feet, because every speed on this dash is mph.
double distance_ft(LatLon a, LatLon b);

// Initial bearing from a to b, degrees clockwise from true north, 0..360.
double bearing_deg(LatLon from, LatLon to);

// Smallest signed difference between two headings, -180..180.
double heading_delta_deg(double from_deg, double to_deg);

struct Crossing {
    bool   crossed  = false;
    double fraction = 0.0;   // where along prev->cur the line was cut, 0..1
    double lateral_ft = 0.0; // offset from the line's centre at the cut
};

// Did the segment prev->cur cross the finish line?
//
// The line is defined by a point, the heading a rider travels when they
// cross it, and a half width. Crossing is a real sign change of the
// along-track distance, not a proximity radius — a radius test fires when
// you ride past the line in the paddock, this does not.
//
// `fraction` matters: at 10 Hz GPS and 60 mph you move 8.8 ft per fix, so
// interpolating the crossing instant is worth about 100 ms of lap-time
// accuracy. Use it.
//
// NOT this function's job: rejecting GPS jitter. A parked bike wanders
// several feet, which is real motion geometrically. A guard big enough to
// swallow that would also swallow a genuine 3 mph crossing. The lap timer
// owns this — gate crossing detection on a minimum speed and a minimum
// lap gap. See test_geo.cpp, the CONTRACT case.
Crossing crosses_line(LatLon prev, LatLon cur, LatLon line,
                      double line_heading_deg, double half_width_ft);

}  // namespace core::geo
