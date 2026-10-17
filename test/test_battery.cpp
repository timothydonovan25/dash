#include "check.hpp"
#include "core/battery/battery.hpp"

using namespace core::battery;

namespace {
// Feed a steady VIN and rpm for `ms` at 100 Hz, as `power` does.
uint32_t hold(Monitor& m, const Config& c, uint32_t t, uint32_t ms, float v, float rpm) {
    for (uint32_t end = t + ms; t < end; t += 10) m.update(v, rpm, t, c);
    return t;
}
}  // namespace

int main() {
    const Config c;

    CASE("on USB nothing is learned");
    {
        Monitor m;
        hold(m, c, 0, 20000, 4.6f, 0.0f);
        CHECK(m.report().verdict == Verdict::Unknown);
        CHECK(m.report().rest_v == 0.0f);
    }

    CASE("rest is read at key-on, after the fuel pump's prime");
    {
        // Key on: the pump pulls the battery down for its two-second prime,
        // then the battery sits at its resting level until the starter.
        Monitor m;
        uint32_t t = hold(m, c, 0, 2000, 11.9f, 0.0f);     // the prime
        t = hold(m, c, t, 900, 12.55f, 0.0f);
        CHECK(m.report().rest_v == 0.0f);                 // window not yet open
        t = hold(m, c, t, 1200, 12.55f, 0.0f);
        CHECK_NEAR(m.report().rest_v, 12.55, 0.01);       // the pump left out
        CHECK(m.report().verdict == Verdict::Good);

        Monitor low;
        hold(low, c, 0, 5000, 12.0f, 0.0f);
        CHECK(low.report().verdict == Verdict::Weak);
    }

    CASE("after a ride the surface charge is not taken for rest");
    {
        // A battery at 12.1 V rests that low at key-on. After a ride it reads
        // 13.0 for an hour off the charge it was just given; read then, it
        // called a half-flat battery good.
        Monitor m;
        uint32_t t = hold(m, c, 0, 5000, 12.1f, 0.0f);
        t = hold(m, c, t, 700, 10.4f, 0.0f);              // the starter
        t = hold(m, c, t, 60000, 14.1f, 4000.0f);          // the ride
        t = hold(m, c, t, 120000, 13.0f, 0.0f);            // parked, key still on
        CHECK_NEAR(m.report().rest_v, 12.1, 0.01);
        CHECK(m.report().verdict == Verdict::Weak);
    }

    CASE("a start inside the rest window spoils it rather than reading the load");
    {
        Monitor m;
        uint32_t t = hold(m, c, 0, 3300, 12.6f, 0.0f);     // window just open
        t = hold(m, c, t, 400, 10.2f, 0.0f);               // starter, no edge yet
        t = hold(m, c, t, 5000, 13.8f, 1600.0f);
        CHECK(m.report().rest_v == 0.0f);
        CHECK(m.report().crank_min_v > 10.0f);
        CHECK(m.report().verdict == Verdict::Good);         // from the crank alone
    }

    CASE("the dash restarting mid-ride takes no rest reading");
    {
        Monitor m;
        hold(m, c, 0, 20000, 13.9f, 5000.0f);
        CHECK(m.report().rest_v == 0.0f);
    }

    CASE("each key-on reads rest again");
    {
        Monitor m;
        uint32_t t = hold(m, c, 0, 5000, 12.6f, 0.0f);
        CHECK_NEAR(m.report().rest_v, 12.6, 0.01);
        t = hold(m, c, t, 2000, 0.0f, 0.0f);               // key off
        hold(m, c, t, 5000, 12.1f, 0.0f);                  // next morning
        CHECK_NEAR(m.report().rest_v, 12.1, 0.01);
        CHECK(m.report().verdict == Verdict::Weak);
    }

    CASE("rest and charge read the mean; the crank dip reads the lowest");
    {
        // Ripple: each 10 ms dips 0.6 V below its mean. Fed the lowest, rest
        // read 12.0 (weak) and charge 13.6 for a battery at 12.6 being
        // charged at 14.2.
        Monitor m;
        uint32_t t = 0;
        for (; t < 5000; t += 10) m.update(12.6f, 12.0f, 0.0f, t, c);
        CHECK_NEAR(m.report().rest_v, 12.6, 0.01);
        for (uint32_t e = t + 600; t < e; t += 10) m.update(10.4f, 9.9f, 0.0f, t, c);
        for (uint32_t e = t + 10000; t < e; t += 10) m.update(14.2f, 13.6f, 4000.0f, t, c);
        CHECK_NEAR(m.report().charge_v, 14.2, 0.05);
        CHECK(m.report().crank_min_v < 10.0f);            // the lowest, smoothed
        CHECK(m.report().verdict == Verdict::Good);
    }

    CASE("the crank dip counts from before the first tach edge");
    {
        Monitor m;
        uint32_t t = hold(m, c, 0, 12000, 12.6f, 0.0f);
        t = hold(m, c, t, 600, 10.1f, 0.0f);              // starter, no edge yet
        t = hold(m, c, t, 300, 10.6f, 350.0f);            // turning over
        t = hold(m, c, t, 5000, 13.8f, 1600.0f);          // running
        CHECK_NEAR(m.report().crank_min_v, 10.1, 0.02);
        CHECK(m.report().verdict == Verdict::Good);
    }

    CASE("a deep crank dip is a weak battery even if it rests fine");
    {
        Monitor m;
        uint32_t t = hold(m, c, 0, 12000, 12.6f, 0.0f);
        t = hold(m, c, t, 700, 9.1f, 0.0f);              // sags for the whole crank
        t = hold(m, c, t, 5000, 13.8f, 1600.0f);
        CHECK_NEAR(m.report().crank_min_v, 9.1, 0.02);
        CHECK(m.report().verdict == Verdict::Weak);
    }

    CASE("the starter's inrush is not the crank voltage");
    {
        // A healthy battery: 50 ms of inrush to 9.0 V, then 10.2 V held
        // while it turns over. Read raw, that was 9.0 and weak.
        Monitor m;
        uint32_t t = hold(m, c, 0, 12000, 12.6f, 0.0f);
        t = hold(m, c, t, 50, 9.0f, 0.0f);
        t = hold(m, c, t, 300, 10.2f, 0.0f);
        t = hold(m, c, t, 500, 10.2f, 300.0f);
        t = hold(m, c, t, 5000, 13.8f, 1600.0f);
        CHECK(m.report().crank_min_v > 10.0f);
        CHECK(m.report().crank_min_v < 10.4f);
        CHECK(m.report().verdict == Verdict::Good);
    }

    CASE("charging is read only held above the charge rpm");
    {
        Monitor m;
        uint32_t t = hold(m, c, 0, 2000, 14.2f, 4000.0f);
        CHECK(m.report().charge_v == 0.0f);               // not yet settled
        t = hold(m, c, t, 5000, 14.2f, 4000.0f);
        CHECK_NEAR(m.report().charge_v, 14.2, 0.05);
        CHECK(m.report().verdict == Verdict::Good);
        hold(m, c, t, 5000, 14.2f, 2000.0f);              // below: unchanged
        CHECK_NEAR(m.report().charge_v, 14.2, 0.05);
    }

    CASE("a dead stator and a dead regulator are told apart");
    {
        Monitor low, high;
        hold(low, c, 0, 20000, 12.4f, 4500.0f);
        CHECK(low.report().verdict == Verdict::NotCharging);
        hold(high, c, 0, 20000, 15.6f, 4500.0f);
        CHECK(high.report().verdict == Verdict::Overcharging);
    }

    return check::report("battery");
}
