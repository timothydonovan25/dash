// Where the sun is, and how bright the backlight should be because of it.
//
// There is no light sensor on the board (§18), but the GPS already says
// where and when, and that is enough to tell day from night. Pure
// arithmetic, so it is tested on the host; the keeper only asks for a
// percentage.
#pragma once
#include <cstdint>

namespace core::sun {

// Sun elevation above the horizon in degrees (negative below it), good to
// about a degree, which is plenty for a ramp that spans six of them.
// `day_of_year` is 1..366, `utc_ms` is time of day (as Gnss::utc_ms is).
float elevation_deg(uint16_t day_of_year, uint32_t utc_ms,
                    double lat_deg, double lon_deg);

struct DimCurve {
    // Full brightness at and above `day_deg`; the floor at and below
    // `night_deg`; a straight line between. 0 to -6 is civil twilight, the
    // time of day street lights come on.
    float day_deg   = 0.0f;
    float night_deg = -6.0f;
    // The share of the rider's brightness setting kept at night.
    uint8_t night_pct = 35;
};

// 100 down to `night_pct`, as a multiplier on the brightness setting.
uint8_t dim_pct(float elevation_deg, const DimCurve& c = DimCurve{});

// The rider's setting scaled by `dim_pct`, never rounded down to dark: a
// setting above zero stays at least 1 %.
uint8_t apply_dim(uint8_t setting_pct, uint8_t dim_pct);

// Holds the last answer through a GPS dropout, because a tunnel at noon must
// not turn the screen down and a tunnel at midnight must not turn it up.
// Until the first fix with a date it answers 100: an unknown sky is a bright
// one, and a dash that is too bright for a minute is better than one that is
// too dark to read.
class AutoDim {
public:
    // Call once a second or so. `have_time` is false when the GPS has no fix
    // or no date yet.
    uint8_t update(bool have_time, uint16_t day_of_year, uint32_t utc_ms,
                   double lat_deg, double lon_deg, bool enabled);
    uint8_t pct() const { return pct_; }

private:
    uint8_t pct_ = 100;
};

}  // namespace core::sun
