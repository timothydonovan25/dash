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


    // --- FailDetector ----------------------------------------------------
    using E = FailDetector::Event;
    auto run = [](FailDetector& d, float v, int n) {
        E last = E::None;
        for (int i = 0; i < n; ++i) {
            const E e = d.feed(v);
            if (e != E::None) last = e;
        }
        return last;
    };

    CASE("USB power never trips, however long it is watched");
    {
        FailDetector d;
        CHECK(run(d, 4.5f, 5000) == E::None);
        CHECK(!d.armed());
        CHECK(run(d, 0.0f, 100) == E::None);
    }

    CASE("it arms on a bike supply and trips on a cut, once");
    {
        FailDetector d;
        run(d, 13.8f, 50);
        CHECK(d.armed());
        // 13.8 V falling at 5 V/ms on a 9 ms hold-up.
        int tripped_at = -1, events = 0;
        float v = 13.8f;
        for (int ms = 0; ms < 10; ++ms, v -= 1.0f) {
            if (d.feed(v) == E::Tripped) { ++events; tripped_at = ms; }
        }
        CHECK(events == 1);
        // 13.8, 12.8 ... first sample below 9.0 is ms 5 (8.8 V), confirmed at ms 6.
        CHECK(tripped_at == 6);
        CHECK(d.tripped());
    }

    CASE("a cranking dip above the trip voltage does nothing");
    {
        FailDetector d;
        run(d, 12.6f, 50);
        CHECK(run(d, 9.6f, 500) == E::None);
        CHECK(d.armed());
    }

    CASE("one low sample is noise, not a cut");
    {
        FailDetector d;
        run(d, 13.8f, 50);
        CHECK(d.feed(5.0f) == E::None);
        CHECK(run(d, 13.8f, 10) == E::None);
        CHECK(d.feed(5.0f) == E::None);
        CHECK(d.armed());
    }

    CASE("a trip that comes back hands everything back");
    {
        FailDetector d;
        run(d, 13.8f, 50);
        CHECK(run(d, 6.0f, 5) == E::Tripped);
        CHECK(run(d, 12.0f, 10) == E::None);            // not yet convincing
        CHECK(d.tripped());
        CHECK(run(d, 12.0f, 60) == E::Recovered);
        CHECK(d.armed());
        CHECK(run(d, 6.0f, 5) == E::Tripped);           // and it can trip again
    }

    CASE("still alive a second after a trip means USB: let go, and stay quiet");
    {
        FailDetector d;
        run(d, 13.8f, 50);
        CHECK(run(d, 6.0f, 5) == E::Tripped);
        CHECK(run(d, 4.5f, 2000) == E::Recovered);
        CHECK(!d.tripped());
        CHECK(!d.armed());
        CHECK(run(d, 4.5f, 2000) == E::None);
        // Plugged back into the bike, it arms again.
        run(d, 13.8f, 50);
        CHECK(d.armed());
    }

    return check::report("power");
}
