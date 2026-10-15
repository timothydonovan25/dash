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

    CASE("a settled battery with the engine off is its rest voltage");
    {
        Monitor m;
        uint32_t t = hold(m, c, 0, 5000, 12.6f, 0.0f);
        CHECK(m.report().rest_v == 0.0f);                 // not settled yet
        t = hold(m, c, t, 10000, 12.6f, 0.0f);
        CHECK_NEAR(m.report().rest_v, 12.6, 0.01);
        CHECK(m.report().verdict == Verdict::Good);
        hold(m, c, t, 60000, 11.9f, 0.0f);                 // run down
        CHECK(m.report().verdict == Verdict::Weak);
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
