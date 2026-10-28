#include "check.hpp"
#include "core/alert/alert.hpp"

using namespace core::alert;
using core::state::Dev;
using core::state::DevState;
using core::state::Frame;

namespace {

// A healthy bike, parked-ish, on a 14 V supply with the engine at idle.
Frame healthy(uint32_t t) {
    Frame f;
    f.t_ms = t;
    f.elec.vbat = 14.0f;
    f.engine.rpm = 1500.0f;
    f.health[Dev::Imu].state = DevState::Ok;
    f.gnss.fix = true;
    return f;
}

// Step a scenario forward in 20 ms frames and return the last answer.
template <class Mut>
Kind run(Alerter& a, uint32_t from, uint32_t ms, Mut mut) {
    Kind k = Kind::None;
    for (uint32_t t = from; t < from + ms; t += 20) {
        Frame f = healthy(t);
        mut(f);
        k = a.update(f);
    }
    return k;
}

}  // namespace

int main() {
    CASE("a healthy bike says nothing");
    {
        Alerter a;
        CHECK(run(a, 0, 30000, [](Frame&) {}) == Kind::None);
    }

    CASE("a fault must last its onset before it shows");
    {
        Alerter a;
        auto sd = [](Frame& f) { f.health[Dev::Sd].state = DevState::Fault; };
        CHECK(run(a, 0, 300, sd) == Kind::None);
        CHECK(run(a, 300, 400, sd) == Kind::SdCard);
    }

    CASE("a flicker restarts the onset instead of accumulating");
    {
        Alerter a;
        uint32_t t = 0;
        for (int i = 0; i < 20; ++i) {
            run(a, t, 200, [](Frame& f) { f.elec.vbat = 17.0f; });       // 200 ms bad
            t += 200;
            CHECK(run(a, t, 200, [](Frame&) {}) == Kind::None);          // 200 ms good
            t += 200;
        }
    }

    CASE("it clears after the condition has gone for a while, not at the first good frame");
    {
        Alerter a;
        auto hi = [](Frame& f) { f.elec.vbat = 17.0f; };
        CHECK(run(a, 0, 4000, hi) == Kind::BatteryHigh);
        CHECK(run(a, 4000, 200, [](Frame&) {}) == Kind::BatteryHigh);    // still up
        CHECK(run(a, 4200, 2000, [](Frame&) {}) == Kind::None);
    }

    CASE("a dismissed alert stays dismissed until its condition has really cleared");
    {
        Alerter a;
        auto dead = [](Frame& f) { f.health[Dev::LedDriver].state = DevState::Fault; };
        CHECK(run(a, 0, 3000, dead) == Kind::ShiftLamps);
        a.acknowledge();
        CHECK(run(a, 3000, 20000, dead) == Kind::None);                  // pressed, so quiet
        CHECK(run(a, 23000, 3000, [](Frame&) {}) == Kind::None);         // fixed
        CHECK(run(a, 26000, 3000, dead) == Kind::ShiftLamps);            // and broken again
    }

    CASE("the most serious alert wins, and a dismissed one lets the next show");
    {
        Alerter a;
        auto both = [](Frame& f) {
            f.health[Dev::LedDriver].state = DevState::Fault;
            f.health[Dev::Sd].state = DevState::Fault;
            f.att.lean_deg = -85.0f;
            f.motion.best_mph = 0.0f;
        };
        CHECK(run(a, 0, 4000, both) == Kind::BikeDown);
        a.acknowledge();
        CHECK(run(a, 4000, 200, both) == Kind::SdCard);                  // before ShiftLamps in the order
        a.acknowledge();
        CHECK(run(a, 4200, 200, both) == Kind::ShiftLamps);
    }

    CASE("bike down needs the lean AND stillness AND a working IMU");
    {
        Alerter a;
        CHECK(run(a, 0, 5000, [](Frame& f) { f.att.lean_deg = 75; f.motion.best_mph = 40; }) == Kind::None);
        Alerter b;
        CHECK(run(b, 0, 5000, [](Frame& f) { f.att.lean_deg = 40; f.motion.best_mph = 0; }) == Kind::None);
        Alerter c;
        CHECK(run(c, 0, 5000, [](Frame& f) { f.att.lean_deg = 75; f.health[Dev::Imu].state = DevState::Fault; })
              == Kind::Imu);   // the IMU fault shows; the dead IMU's lean is not believed
        Alerter d;
        CHECK(run(d, 0, 1000, [](Frame& f) { f.att.lean_deg = 75; }) == Kind::None);   // a wheelie recovery
        CHECK(run(d, 1000, 2000, [](Frame& f) { f.att.lean_deg = 75; }) == Kind::BikeDown);
    }

    CASE("a low battery means something only when the engine should be charging");
    {
        Alerter a;
        CHECK(run(a, 0, 10000, [](Frame& f) { f.elec.vbat = 11.5f; f.engine.rpm = 1200; }) == Kind::None);
        CHECK(run(a, 10000, 10000, [](Frame& f) { f.elec.vbat = 11.5f; f.engine.rpm = 4000; }) == Kind::BatteryLow);
    }

    CASE("on USB, or with no reading, there are no battery alerts");
    {
        Alerter a;
        CHECK(run(a, 0, 20000, [](Frame& f) { f.elec.vbat = 4.5f; f.engine.rpm = 4000; }) == Kind::None);
        CHECK(run(a, 20000, 20000, [](Frame& f) { f.elec.vbat = 0.0f; f.engine.rpm = 4000; }) == Kind::None);
    }

    CASE("losing GPS matters while timing a lap, and not otherwise");
    {
        Alerter a;
        CHECK(run(a, 0, 20000, [](Frame& f) { f.gnss.fix = false; }) == Kind::None);
        Alerter b;
        auto timing = [](Frame& f) { f.gnss.fix = false; f.lap.phase = core::lap::Phase::Timing; };
        CHECK(run(b, 0, 3000, timing) == Kind::None);                    // a tunnel
        CHECK(run(b, 3000, 4000, timing) == Kind::GpsLost);
    }

    CASE("no pulse from a watch worn this ride: 10 s, then over everything, until a pulse");
    {
        // The watch, connected, readings twice a second, none with a pulse.
        auto off_wrist = [](Frame& f) {
            f.heart.link = core::state::Heart::Live;
            f.heart.worn = true;
            f.heart.bpm = 0;
            f.heart.at_ms = f.t_ms - (f.t_ms % 500);
        };
        Alerter a;
        CHECK(run(a, 0, 9000, off_wrist) == Kind::None);   // a wrist shifting, a watch catching up
        CHECK(run(a, 9000, 2000, off_wrist) == Kind::NoPulse);
        // Over the bike on its side too: the rider comes first.
        auto both = [&](Frame& f) { off_wrist(f); f.att.lean_deg = 80; };
        CHECK(run(a, 11000, 3000, both) == Kind::NoPulse);
        // A pulse again, and it clears.
        auto worn = [](Frame& f) {
            f.heart.link = core::state::Heart::Live;
            f.heart.worn = true;
            f.heart.bpm = 92;
            f.heart.at_ms = f.t_ms - (f.t_ms % 500);
        };
        CHECK(run(a, 14000, 3000, worn) == Kind::None);

        // Never worn on this link, or a watch gone quiet: nothing.
        Alerter b;
        CHECK(run(b, 0, 20000, [&](Frame& f) { off_wrist(f); f.heart.worn = false; }) == Kind::None);
        Alerter c;
        CHECK(run(c, 0, 20000, [&](Frame& f) { off_wrist(f); f.heart.at_ms = 1; }) == Kind::None);
    }

    CASE("a dead wheel sensor names its wheel");
    {
        Alerter a;
        CHECK(run(a, 0, 1000, [](Frame& f) { f.health[Dev::RearWheel].state = DevState::Fault; }) == Kind::RearWheel);
    }

    CASE("every kind has text, and None has none");
    {
        for (uint8_t i = 1; i < static_cast<uint8_t>(Kind::Count); ++i) {
            CHECK(text(static_cast<Kind>(i))[0] != '\0');
        }
        CHECK(text(Kind::None)[0] == '\0');
    }

    return check::report("alert");
}
