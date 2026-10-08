#include "check.hpp"
#include "core/power/power.hpp"

#include <cmath>

using namespace core::power;

int main() {
    const Divider div;   // 100k over 22.1k, R17 and R18

    CASE("the divider scales the pin back up to VIN");
    {
        // 14.4 V across 100k/22.1k is 2.606 V at the pin.
        CHECK_NEAR(vin_from_pin_mv(2606, div), 14.4, 0.01);
        CHECK_NEAR(vin_from_pin_mv(814, div), 4.50, 0.01);
        CHECK_NEAR(vin_from_pin_mv(0, div), 0.0, 1e-6);
    }

    CASE("a divider with no bottom resistor reads nothing rather than dividing by zero");
    {
        Divider broken;
        broken.bottom_kohm = 0.0f;
        CHECK(vin_from_pin_mv(2000, broken) == 0.0f);
    }

    CASE("USB through the Schottky reads as USB");
    {
        CHECK(classify(4.5f) == Source::Usb);
        CHECK(classify(4.2f) == Source::Usb);   // a sagging port
    }

    CASE("the bike reads as the bike from a flat battery to a charging one");
    {
        CHECK(classify(11.5f) == Source::Bike);
        CHECK(classify(12.6f) == Source::Bike);
        CHECK(classify(14.4f) == Source::Bike);
    }

    CASE("no reading is Unknown, not USB");
    {
        // An unfitted divider or a dead ADC reads near zero. Calling that
        // USB would cap the shift lamps on a running bike.
        CHECK(classify(0.0f) == Source::Unknown);
        CHECK(classify(1.0f) == Source::Unknown);
        CHECK(classify(std::nanf("")) == Source::Unknown);
    }

    CASE("the two thresholds are where the edges fall");
    {
        const Thresholds t;
        CHECK(classify(t.present_v, t) == Source::Usb);
        CHECK(classify(t.bike_v, t) == Source::Bike);
        CHECK(classify(t.bike_v - 0.01f, t) == Source::Usb);
    }

    CASE("lamp brightness: the setting on the bike, capped on USB");
    {
        CHECK(lamp_master_pct(true, 100, 30) == 100);
        CHECK(lamp_master_pct(true, 40, 30) == 40);
        CHECK(lamp_master_pct(false, 100, 30) == 30);   // cap wins
        CHECK(lamp_master_pct(false, 20, 30) == 20);    // setting can go lower
        CHECK(lamp_master_pct(true, 250, 30) == 100);
    }

    return check::report("power");
}
