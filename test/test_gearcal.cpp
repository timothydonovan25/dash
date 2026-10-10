#include "check.hpp"
#include "core/gearcal/gearcal.hpp"

using namespace core::gearcal;

namespace {

// The bike's table, as the firmware ships it.
const float kTable[kMaxGears] = {420.0f, 290.0f, 215.0f, 165.0f, 130.0f, 0.0f};

// A deterministic bit of jitter, +-1 in `amp` units, so a "steady" ride is
// not suspiciously perfect.
float jit(uint32_t t, float amp) {
    const uint32_t h = (t * 2654435761u) >> 20;
    return (static_cast<float>(h % 2001u) / 1000.0f - 1.0f) * amp;
}

struct Bike {
    uint32_t t = 1000;
    float    rpm = 0, mph = 0, gps = 0;
    bool     fix = true, neutral = false;
    float    hdop = 1.0f;

    Sample sample() const {
        Sample s;
        s.t_ms = t;
        s.rpm = rpm * (1.0f + jit(t, 0.004f));
        s.road_mph = mph * (1.0f + jit(t + 7, 0.008f));
        s.gps_mph = gps;
        s.gps_ok = fix;
        s.hdop = hdop;
        s.neutral = neutral;
        return s;
    }
};

// Ride at a steady point for `ms`, feeding `f` every 20 ms.
template <typename F>
void hold(Bike& b, uint32_t ms, F&& f) {
    for (uint32_t i = 0; i < ms; i += 20) {
        f(b.sample());
        b.t += 20;
    }
}

// The bike is in gear `g`, whose true ratio is table * (1 + err), at `rpm`.
void in_gear(Bike& b, uint8_t g, float err, float rpm, float gps_scale = 1.0f) {
    const float ratio = kTable[g - 1] * (1.0f + err);
    b.rpm = rpm;
    b.mph = rpm / ratio;
    b.gps = b.mph * gps_scale;
}

}  // namespace

int main() {
    CASE("a steady ride builds a window; its ratio is rpm over road speed");
    {
        Window w;
        Bike b;
        in_gear(b, 3, 0.0f, 6000.0f);
        SteadyConfig c;
        hold(b, 11000, [&](const Sample& s) { CHECK(w.feed(s, c) != Reason::GpsApart); });
        CHECK(w.duration_ms() >= 10000);
        CHECK_NEAR(w.ratio(), 215.0, 2.0);
        CHECK_NEAR(w.gps_over_road(), 0.0, 0.01);
    }

    CASE("neutral, low revs, no GPS each keep a window from forming");
    {
        Window w;
        Bike b;
        SteadyConfig c;
        in_gear(b, 3, 0.0f, 6000.0f);
        b.neutral = true;
        hold(b, 3000, [&](const Sample& s) { CHECK(w.feed(s, c) == Reason::Neutral); });
        CHECK(w.duration_ms() == 0);
        b.neutral = false;
        b.fix = false;
        hold(b, 3000, [&](const Sample& s) { CHECK(w.feed(s, c) == Reason::NoGps); });
        b.fix = true;
        b.hdop = 6.0f;
        hold(b, 1000, [&](const Sample& s) { CHECK(w.feed(s, c) == Reason::NoGps); });
        b.hdop = 1.0f;
        in_gear(b, 3, 0.0f, 1500.0f);
        hold(b, 1000, [&](const Sample& s) { CHECK(w.feed(s, c) == Reason::Revs); });
        CHECK(w.duration_ms() == 0);
    }

    CASE("accelerating restarts the window, so a gentle pull never counts");
    {
        Window w;
        Bike b;
        SteadyConfig c;
        in_gear(b, 3, 0.0f, 4000.0f);
        uint32_t longest = 0;
        // 4000 to 8000 rpm over 20 s is a 20 % change per 5 s.
        for (uint32_t i = 0; i < 20000; i += 20) {
            const float rpm = 4000.0f + 4000.0f * i / 20000.0f;
            in_gear(b, 3, 0.0f, rpm);
            w.feed(b.sample(), c);
            if (w.duration_ms() > longest) longest = w.duration_ms();
            b.t += 20;
        }
        CHECK(longest < 5000);
    }

    CASE("a rear wheel spinning against GPS is thrown out");
    {
        Window w;
        Bike b;
        SteadyConfig c;
        in_gear(b, 2, 0.0f, 6000.0f);
        hold(b, 4000, [&](const Sample& s) { w.feed(s, c); });
        CHECK(w.duration_ms() >= 3000);
        // The wheel runs 15 % fast against a GPS that has not moved.
        b.mph *= 1.15f;
        Reason last = Reason::Steady;
        hold(b, 1500, [&](const Sample& s) { last = w.feed(s, c); });
        CHECK(last == Reason::GpsApart);
        CHECK(w.duration_ms() == 0);
    }

    CASE("a gap in the samples does not bridge a window");
    {
        Window w;
        Bike b;
        SteadyConfig c;
        in_gear(b, 4, 0.0f, 6000.0f);
        hold(b, 5000, [&](const Sample& s) { w.feed(s, c); });
        CHECK(w.duration_ms() >= 4000);
        b.t += 5000;
        w.feed(b.sample(), c);
        CHECK(w.duration_ms() == 0);
    }

    CASE("the guided run: 10 s steady, told OK, shift up moves on by itself");
    {
        Guide g;
        g.open(5, kTable);
        CHECK(g.phase() == Phase::Intro);
        CHECK(!g.next());
        CHECK(g.phase() == Phase::Measuring);
        CHECK(g.gear() == 1);

        Bike b;
        // 1st is 2 % high, the rest as per the table.
        in_gear(b, 1, 0.02f, 6000.0f);
        bool ready_early = false;
        hold(b, 8000, [&](const Sample& s) { g.step(s); ready_early |= g.ready(); });
        CHECK(!ready_early);
        CHECK(!g.next());                       // E before ready does nothing
        CHECK(g.gear() == 1);

        hold(b, 4000, [&](const Sample& s) { g.step(s); });
        CHECK(g.ready());
        CHECK_NEAR(g.live_ratio(), 420.0 * 1.02, 3.0);
        CHECK_NEAR(g.live_err(), 0.02, 0.01);

        // Change up: rpm drops to what 2nd gives at the same road speed.
        const float mph = b.mph;
        b.rpm = mph * kTable[1];
        hold(b, 1500, [&](const Sample& s) { g.step(s); });
        CHECK(g.gear() == 2);
        CHECK(g.has_result(1));
        CHECK_NEAR(g.result_err(1), 0.02, 0.01);
        CHECK(!g.ready());
    }

    CASE("the whole run, apply, and the results");
    {
        Guide g;
        g.open(5, kTable);
        g.next();
        Bike b;
        const float errs[5] = {-0.02f, -0.06f, -0.065f, -0.055f, -0.01f};
        for (uint8_t gear = 1; gear <= 5; ++gear) {
            in_gear(b, gear, errs[gear - 1], gear < 3 ? 6000.0f : 7000.0f);
            hold(b, 11000, [&](const Sample& s) { g.step(s); });
            CHECK(g.ready());
            if (gear < 5) {
                // Shift up on a real bike: the next gear's rpm at the same
                // speed, then a moment later it settles there.
                const float mph = b.mph;
                b.rpm = mph * kTable[gear] * (1.0f + errs[gear]);
                hold(b, 1500, [&](const Sample& s) { g.step(s); });
                CHECK(g.gear() == gear + 1);
                // The shift carries the speed along; start the next gear's
                // steady stretch from where it landed.
                b.t += 0;
            } else {
                CHECK(g.phase() == Phase::Measuring);
                CHECK(!g.next());
                CHECK(g.phase() == Phase::Summary);
            }
        }
        for (uint8_t gear = 1; gear <= 5; ++gear) {
            CHECK(g.has_result(gear));
            CHECK_NEAR(g.result_err(gear), errs[gear - 1], 0.012);
        }
        CHECK(!g.circ_valid());                 // GPS agreed with the wheel
        CHECK(g.next());                        // accepted
        CHECK(g.phase() == Phase::Idle);
        CHECK_NEAR(g.result_ratio(3), 215.0 * (1.0 - 0.065), 3.0);   // still readable
    }

    CASE("riding the wrong gear is called out, and does not count");
    {
        Guide g;
        g.open(5, kTable);
        g.next();
        g.next();
        Bike b;
        in_gear(b, 3, 0.0f, 6000.0f);           // asked for 1st, in 3rd
        hold(b, 12000, [&](const Sample& s) { g.step(s); });
        CHECK(g.reason() == Reason::WrongGear);
        CHECK(g.nearest() == 3);
        CHECK(!g.ready());
    }

    CASE("a wheel that disagrees with GPS is reported as a circumference");
    {
        Guide g;
        g.open(2, kTable);
        g.next();
        Bike b;
        // The wheel reads 3 % low: GPS is 3 % above it.
        in_gear(b, 1, 0.0f, 6000.0f, 1.03f);
        hold(b, 11000, [&](const Sample& s) { g.step(s); });
        CHECK(g.ready());
        CHECK(!g.next());                       // keep 1st
        CHECK(g.gear() == 2);
        g.skip();                               // leave 2nd out
        CHECK(g.phase() == Phase::Summary);
        CHECK(g.circ_valid());
        CHECK_NEAR(g.circ_err(), 0.03, 0.005);
        CHECK(!g.has_result(2));
    }

    CASE("cancel throws the run away");
    {
        Guide g;
        g.open(5, kTable);
        g.next();
        Bike b;
        in_gear(b, 1, 0.0f, 6000.0f);
        hold(b, 11000, [&](const Sample& s) { g.step(s); });
        CHECK(g.ready());
        g.cancel();
        CHECK(g.phase() == Phase::Idle);
        CHECK(!g.next());
    }

    // --- the learner ----------------------------------------------------

    // One steady stretch of `ms` in gear 3 that is `err` off the table.
    auto stretch = [](Learner& l, Bike& b, float err, uint32_t ms, float rpm,
                      uint8_t* gear, float* value) {
        in_gear(b, 3, err, rpm);
        bool moved = false;
        hold(b, ms, [&](const Sample& s) {
            moved |= l.step(s, kTable, kTable, 5, *gear, *value);
        });
        return moved;
    };
    // A break between stretches: a roll-off that kills the window and then
    // the 30 s the learner wants between episodes.
    auto gap = [](Learner& l, Bike& b, uint32_t ms) {
        b.neutral = true;
        uint8_t g = 0; float v = 0;
        hold(b, ms, [&](const Sample& s) { l.step(s, kTable, kTable, 5, g, v); });
        b.neutral = false;
    };

    CASE("the learner notes the error but does nothing for a long time");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        // Seven episodes, all 5 % low in the table's favour: not yet eight.
        for (int i = 0; i < 7; ++i) {
            CHECK(!stretch(l, b, 0.05f, 7000, 6000.0f, &gear, &value));
            gap(l, b, 31000);
        }
        CHECK(l.episodes(3) == 7);
        CHECK_NEAR(l.error(3), 0.05, 0.01);
        CHECK(!l.adjusted(3));
    }

    CASE("eight agreeing episodes nudge one entry by 1 %, once");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        bool moved = false;
        for (int i = 0; i < 8; ++i) {
            moved |= stretch(l, b, 0.05f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 31000);
        }
        CHECK(moved);
        CHECK(gear == 3);
        // 5 % off, but a move is capped at 1 %: 215 -> 217.2 (rounded).
        CHECK_NEAR(value, 215.0 * 1.01, 0.2);
        CHECK(l.adjusted(3));
        CHECK(l.adjusted_mask() == 0x04);

        // And never again this power cycle, however much more evidence.
        bool again = false;
        for (int i = 0; i < 10; ++i) {
            again |= stretch(l, b, 0.05f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 31000);
        }
        CHECK(!again);
    }

    CASE("one long steady cruise is one episode, not eight");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        stretch(l, b, 0.05f, 120000, 6000.0f, &gear, &value);
        CHECK(l.episodes(3) == 1);
    }

    CASE("episodes closer than 30 s in one gear are the same riding");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        for (int i = 0; i < 6; ++i) {
            stretch(l, b, 0.05f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 3000);
        }
        // A minute of it, in 10 s pieces: two episodes, not six.
        CHECK(l.episodes(3) == 2);
    }

    CASE("errors that disagree in sign move nothing");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        bool moved = false;
        for (int i = 0; i < 16; ++i) {
            moved |= stretch(l, b, i % 2 ? 0.04f : -0.04f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 31000);
        }
        CHECK(!moved);
    }

    CASE("an error inside the dead band moves nothing");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        bool moved = false;
        for (int i = 0; i < 12; ++i) {
            moved |= stretch(l, b, 0.01f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 31000);
        }
        CHECK(!moved);
        CHECK(l.episodes(3) == 8);
    }

    CASE("a few slipping-clutch episodes cannot drag it past the clean ones");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        bool moved = false;
        // Clean episodes 2 % high, slipped ones 5 % high. The move follows
        // the smallest, and the spread (3 %) is inside the limit.
        for (int i = 0; i < 8; ++i) {
            moved |= stretch(l, b, i < 5 ? 0.02f : 0.05f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 31000);
        }
        CHECK(moved);
        CHECK_NEAR(value, 215.0 * 1.01, 0.2);   // 1 %, never more

        // Spread beyond 4 % is not agreement at all.
        Learner l2;
        Bike b2;
        moved = false;
        for (int i = 0; i < 8; ++i) {
            moved |= stretch(l2, b2, i < 4 ? 0.02f : 0.07f, 7000, 6000.0f, &gear, &value);
            gap(l2, b2, 31000);
        }
        CHECK(!moved);
    }

    CASE("the learner will not move an entry more than 5 % from its base");
    {
        // The table sits 4.5 % above its base already.
        float table[kMaxGears] = {420, 290, 215 * 1.045f, 165, 130, 0};
        Learner l;
        Bike b;
        bool moved = false;
        uint8_t gear = 0; float value = 0;
        b.rpm = 6000.0f;
        for (int i = 0; i < 10; ++i) {
            // The bike reads 6 % above that table, so it wants to go up.
            b.mph = 6000.0f / (table[2] * 1.06f);
            b.gps = b.mph;
            hold(b, 7000, [&](const Sample& s) {
                moved |= l.step(s, table, kTable, 5, gear, value);
            });
            gap(l, b, 31000);
        }
        CHECK(!moved);                          // +1 % would be 5.5 % over base
    }

    CASE("wheelspin and a wrong circumference keep the learner out");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        // The wheel agrees with GPS only to 4 %: outside the learner's 2.5 %.
        in_gear(b, 3, 0.02f, 6000.0f, 1.04f);
        hold(b, 30000, [&](const Sample& s) { l.step(s, kTable, kTable, 5, gear, value); });
        CHECK(l.episodes(3) == 0);
    }

    CASE("an error near no gear is evidence about none");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        // Midway between 3rd and 4th.
        b.rpm = 6000.0f;
        b.mph = 6000.0f / 188.0f;
        b.gps = b.mph;
        hold(b, 30000, [&](const Sample& s) { l.step(s, kTable, kTable, 5, gear, value); });
        CHECK(l.episodes(3) == 0 && l.episodes(4) == 0);
    }

    CASE("resetting forgets the evidence");
    {
        Learner l;
        Bike b;
        uint8_t gear = 0; float value = 0;
        for (int i = 0; i < 3; ++i) {
            stretch(l, b, 0.05f, 7000, 6000.0f, &gear, &value);
            gap(l, b, 31000);
        }
        CHECK(l.episodes(3) == 3);
        l.reset_gear(3);
        CHECK(l.episodes(3) == 0);
    }

    return check::report("gearcal");
}
