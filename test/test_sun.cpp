#include "check.hpp"
#include "core/sun/sun.hpp"

using namespace core::sun;

namespace {
constexpr uint32_t hours(double h) { return static_cast<uint32_t>(h * 3600000.0); }
}  // namespace

int main() {
    CASE("the sun is overhead at the equator at the equinox, at solar noon");
    {
        // Day 80 (20 March), lon 0, noon UTC; the equation of time is about -7 min.
        CHECK_NEAR(elevation_deg(80, hours(12.1), 0.0, 0.0), 90.0, 4.0);
    }

    CASE("midsummer noon at 40 N stands about 73 degrees high");
    {
        // 90 - 40 + 23.44 = 73.4 at solar noon; longitude 0 and noon UTC are within minutes of it.
        CHECK_NEAR(elevation_deg(172, hours(12.0), 40.0, 0.0), 73.4, 1.0);
    }

    CASE("midwinter noon at 40 N is only 26 degrees up");
    {
        CHECK_NEAR(elevation_deg(355, hours(12.0), 40.0, 0.0), 26.6, 1.0);
    }

    CASE("midnight is below the horizon, and the longitude shifts the clock");
    {
        CHECK(elevation_deg(172, hours(0.0), 40.0, 0.0) < -20.0);
        // 05:00 UTC at 75 W is midnight local, not dawn; 17:00 UTC is local noon.
        CHECK(elevation_deg(172, hours(5.0), 40.0, -75.0) < -20.0);
        CHECK(elevation_deg(172, hours(17.0), 40.0, -75.0) > 70.0);
    }

    CASE("sunrise at Greenwich on the March equinox is about 06:00 UTC");
    {
        // At the geometric horizon (refraction would bring it a few minutes earlier).
        CHECK_NEAR(elevation_deg(80, hours(6.0), 51.5, 0.0), 0.0, 3.0);
        CHECK(elevation_deg(80, hours(4.5), 51.5, 0.0) < -6.0);
        CHECK(elevation_deg(80, hours(8.0), 51.5, 0.0) > 15.0);
    }

    CASE("the dim curve is full by day, floored at night, and straight between");
    {
        const DimCurve c;
        CHECK(dim_pct(30.0f, c) == 100);
        CHECK(dim_pct(0.0f, c) == 100);
        CHECK(dim_pct(-6.0f, c) == c.night_pct);
        CHECK(dim_pct(-40.0f, c) == c.night_pct);
        CHECK_NEAR(dim_pct(-3.0f, c), (100 + c.night_pct) / 2.0, 1.0);
        uint8_t last = 100;
        for (float e = 5.0f; e > -10.0f; e -= 0.25f) {     // never brightens as it gets darker
            const uint8_t d = dim_pct(e, c);
            CHECK(d <= last);
            last = d;
        }
    }

    CASE("the setting is scaled, and a lit screen is never rounded to dark");
    {
        CHECK(apply_dim(80, 100) == 80);
        CHECK(apply_dim(80, 35) == 28);
        CHECK(apply_dim(1, 35) == 1);
        CHECK(apply_dim(0, 35) == 0);
    }

    CASE("AutoDim: off means full, unknown means full, a dropout holds the last answer");
    {
        AutoDim a;
        CHECK(a.pct() == 100);
        CHECK(a.update(false, 0, 0, 0, 0, true) == 100);           // no fix yet
        CHECK(a.update(true, 172, hours(0.0), 40.0, 0.0, true) == DimCurve{}.night_pct);
        CHECK(a.update(false, 0, 0, 0, 0, true) == DimCurve{}.night_pct);    // tunnel at night
        CHECK(a.update(true, 172, hours(12.0), 40.0, 0.0, true) == 100);
        CHECK(a.update(false, 0, 0, 0, 0, true) == 100);           // tunnel at noon
        CHECK(a.update(true, 172, hours(0.0), 40.0, 0.0, false) == 100);     // setting off
        CHECK(a.update(true, 0, hours(0.0), 40.0, 0.0, true) == 100);        // fix but no date
    }

    return check::report("sun");
}
