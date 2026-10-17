#include "check.hpp"
#include "core/flash/flash.hpp"

#include <cmath>

using namespace core::flash;

namespace {

Stillness parked() {
    Stillness s;
    s.lamp_base_rpm = 9000;
    return s;
}

}  // namespace

int main() {
    CASE("parked, engine off, no fix: still");
    CHECK(still(parked()));

    CASE("idling is still: the lamps are dark at idle");
    {
        Stillness s = parked();
        s.rpm = 1800;
        CHECK(still(s));
    }

    CASE("revving on a start gate is not still");
    {
        Stillness s = parked();
        s.rpm = 6000;
        CHECK(!still(s));
    }

    CASE("either wheel moving is moving, whichever one the speedo shows");
    {
        Stillness s = parked();
        s.rear_mph = 12.0f;          // front sensor dead, reading 0
        CHECK(!still(s));
        s = parked();
        s.front_mph = 0.6f;
        CHECK(!still(s));
    }

    CASE("GPS catches a bike whose wheel sensors both read 0");
    {
        Stillness s = parked();
        s.gps_fix = true;
        s.gps_mph = 25.0f;
        CHECK(!still(s));
    }

    CASE("GPS wander on a parked bike is still");
    {
        Stillness s = parked();
        s.gps_fix = true;
        s.gps_mph = 1.2f;
        CHECK(still(s));
    }

    CASE("GPS speed without a fix is ignored");
    {
        Stillness s = parked();
        s.gps_fix = false;
        s.gps_mph = 40.0f;
        CHECK(still(s));
    }

    CASE("NaN anywhere reads as moving");
    {
        Stillness s = parked();
        s.front_mph = std::nanf("");
        CHECK(!still(s));
        s = parked();
        s.rpm = std::nanf("");
        CHECK(!still(s));
    }

    CASE("no shift config (base 0): engine off still saves, turning does not");
    {
        Stillness s = parked();
        s.lamp_base_rpm = 0;
        s.rpm = 1200;
        CHECK(!still(s));
        s.rpm = 0;
        CHECK(still(s));
    }

    return check::report("flash");
}
