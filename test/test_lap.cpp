#include "check.hpp"
#include "core/lap/lap.hpp"
#include <cmath>

using namespace core::lap;
using core::geo::LatLon;

namespace {

// The finish line: a point at 40N, crossed heading due north.
const LatLon kLine{40.000000, -90.000000};
const double kNorth = 0.0;

// Degrees per foot in the same flat frame `crosses_line()` uses, so the sim
// and the code under test agree to the last bit.
const double kFtLat = 1.0 / 364000.0;
const double kFtLon = 1.0 / (364000.0 * std::cos(40.0 * 3.14159265358979323846 / 180.0));

LatLon at_ft(double x_east, double y_north) {
    return LatLon{kLine.lat + y_north * kFtLat, kLine.lon + x_east * kFtLon};
}

// A rider on a 2400 ft rectangular circuit, sampled at 10 Hz like the real
// receiver. One step is 8.8 ft, which is 60 mph — the speed this dash was
// built for, and the case where sub-sample interpolation actually matters.
//
// The loop starts 444.4 ft behind the line so that the crossing falls in the
// middle of a fix pair rather than exactly on a fix, and every leg is a whole
// number of steps, so a nominal lap is exactly 30.000 s.
struct Sim {
    explicit Sim(Timer& t) : tm(t) {}

    Timer&   tm;
    Config   cfg{};
    double   x = 0.0;
    double   y = -444.4;
    uint32_t t_ms = 0;
    bool     fix = true;
    float    speed_override = -1.0f;   // < 0 means derive from the step

    int      n_started = 0;
    int      n_completed = 0;
    uint32_t last_lap_ms = 0;
    bool     last_lap_valid = true;

    void step(double dx, double dy) {
        x += dx;
        y += dy;
        t_ms += 100;
        Fix f;
        f.pos = at_ft(x, y);
        f.speed_mph = speed_override >= 0.0f
                          ? speed_override
                          : static_cast<float>(std::hypot(dx, dy) * 10.0 / 1.4666667);
        f.valid = fix;
        f.t_ms = t_ms;
        const Update u = tm.update(f, cfg);
        if (u.event == Event::Started) ++n_started;
        if (u.event == Event::Completed) {
            ++n_completed;
            last_lap_ms = u.lap_ms;
            last_lap_valid = u.valid;
        }
    }

    void leg(int steps, double dx, double dy) {
        for (int i = 0; i < steps; ++i) step(dx, dy);
    }

    // North through the line, east, south, west back to the start.
    void lap() {
        leg(100, 0.0,  8.8);
        leg( 50, 8.8,  0.0);
        leg(100, 0.0, -8.8);
        leg( 50,-8.8,  0.0);
    }
};

}  // namespace

int main() {
    CASE("with no line set, riding over it does nothing");
    {
        Timer t;
        Sim s(t);
        s.lap();
        CHECK(t.state().phase == Phase::NoLine);
        CHECK(s.n_started == 0);
        CHECK(s.n_completed == 0);
    }

    CASE("the first crossing starts the clock but is not a lap");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        CHECK(t.state().phase == Phase::Armed);
        Sim s(t);
        s.lap();
        CHECK(s.n_started == 1);
        CHECK(s.n_completed == 0);
        CHECK(t.state().count == 0);
        CHECK(t.state().phase == Phase::Timing);
        // Started at 5050 ms, ran to 30000 ms.
        CHECK_NEAR(t.state().current_ms, 24950.0, 3.0);
    }

    CASE("the second crossing scores the lap");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.lap();
        s.lap();
        CHECK(s.n_completed == 1);
        CHECK(t.state().count == 1);
        CHECK_NEAR(s.last_lap_ms, 30000.0, 3.0);   // 2400 ft at 88 ft/s
        CHECK(s.last_lap_valid);
        CHECK_NEAR(t.state().last_ms, 30000.0, 3.0);
        CHECK_NEAR(t.state().best_ms, 30000.0, 3.0);
        // The clock restarted at the crossing, 24.95 s before the sim ended.
        CHECK_NEAR(t.state().current_ms, 24950.0, 3.0);
    }

    CASE("a slower lap does not touch the best");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.lap();
        s.lap();                        // 30.000
        s.leg(10, 0.0, 0.0);            // a 1.0 s stop on the back straight
        s.lap();                        // 31.000
        CHECK(t.state().count == 2);
        CHECK_NEAR(t.state().last_ms, 31000.0, 3.0);
        CHECK_NEAR(t.state().best_ms, 30000.0, 3.0);
    }

    CASE("the crossing instant is interpolated inside the fix pair");
    {
        // Lap one crosses halfway between two fixes (t = 5050 ms). The back
        // straight is then shortened by 2.2 ft, so lap two crosses a quarter
        // of the way in (t = 35025 ms). The true lap is 29975 ms. A timer that
        // used the fix timestamp instead would report exactly 30000 — 25 ms
        // out here, and up to 100 ms out at 60 mph in general.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.leg(100, 0.0,  8.8);          // crossing at 5050 ms
        s.leg( 50, 8.8,  0.0);
        s.leg( 99, 0.0, -8.8);
        s.leg(  1, 0.0, -6.6);          // 2.2 ft short of where lap one began
        s.leg( 50,-8.8,  0.0);
        s.leg(100, 0.0,  8.8);          // crossing at 35025 ms
        CHECK(s.n_completed == 1);
        CHECK_NEAR(s.last_lap_ms, 29975.0, 3.0);
    }

    CASE("walking pace does not score a lap");
    {
        // 0.73 ft per fix is 5 mph — above the half-foot guard inside
        // crosses_line(), so the geometry says yes and only the speed gate
        // says no. This is riding into the pits over the timing line.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.y = -7.0;
        s.leg(20, 0.0, 0.73);
        CHECK(s.n_started == 0);
        CHECK(t.state().phase == Phase::Armed);
    }

    CASE("a parked bike never scores, however far its fix wanders");
    {
        // The reason the speed gate reads the receiver's own Doppler speed and
        // not a speed differenced from positions: a stationary consumer
        // receiver reports ~0 mph while its reported position wanders several
        // feet, which is genuine northbound motion as far as the geometry is
        // concerned. This is the CONTRACT case in test_geo.cpp, gated.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.speed_override = 0.4f;        // what a parked receiver actually says
        s.x = 0.0;
        s.y = 0.0;
        for (int n = 0; n < 200; ++n) {
            const double wander = ((n * 37) % 13 - 6) * 0.7;   // +/- 4.2 ft
            s.step(0.0, wander - s.y);          // jump to the wandered position
        }
        CHECK(s.n_started == 0);
        CHECK(s.n_completed == 0);
        CHECK(t.state().phase == Phase::Armed);
    }

    CASE("crossing the line twice in quick succession is one crossing");
    {
        // Rider crosses, doubles back over the line and crosses again five
        // seconds later. The clock must keep running from the first crossing.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.leg(100, 0.0,  8.8);          // arms, crossing at 5050 ms
        CHECK(s.n_started == 1);
        s.y = 40.0;                     // just in front of the line
        s.leg(10, 0.0, -8.8);           // back over it southbound: no trigger
        s.leg(20, 0.0,  8.8);           // northbound again, ~3 s later
        CHECK(s.n_completed == 0);
        CHECK(t.state().count == 0);
        CHECK(t.state().phase == Phase::Timing);
        // Still timing from 5050 ms, not restarted by the re-trigger: the sim
        // ends at 13000 ms, so a restarted clock would read about 2000.
        CHECK_NEAR(t.state().current_ms, 7950.0, 3.0);
    }

    CASE("...and the minimum lap time is what rejected it");
    {
        // The same ride with the gate opened right up does score, which is
        // what proves the case above tested the gate and not the geometry.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.cfg.min_lap_ms = 1000;
        s.leg(100, 0.0,  8.8);
        s.y = 40.0;
        s.leg(10, 0.0, -8.8);
        s.leg(20, 0.0,  8.8);
        CHECK(s.n_completed == 1);
    }

    CASE("losing the fix does not teleport the rider across the line");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.leg(10, 0.0, 8.8);            // approaching, 350 ft to go
        s.fix = false;
        s.leg(40, 0.0, 8.8);            // rides through the line, blind
        s.fix = true;
        s.leg(10, 0.0, 8.8);            // fix returns well past the line
        CHECK(s.n_started == 0);        // no crossing invented on the way back
        CHECK(t.state().phase == Phase::Armed);
        // And the timer is not wedged: the next real crossing still arms it.
        s.leg( 50, 8.8,  0.0);
        s.leg(100, 0.0, -8.8);
        s.leg( 50,-8.8,  0.0);
        s.leg(100, 0.0,  8.8);
        CHECK(s.n_started == 1);
    }

    CASE("a brief dropout across the line invents no crossing");
    {
        // The nastier version of the case above: the fix drops for 1.2 s,
        // which is inside the max fix gap, and the rider covers the 105 ft
        // through the line while it is dark. The last good fix is 48 ft
        // behind the line and the next is 66 ft in front, so pairing them
        // scores a lap the receiver never saw. A tower or a bridge over the
        // start straight does exactly this.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.leg(45, 0.0, 8.8);            // up to 48 ft short of the line
        s.fix = false;
        s.leg(12, 0.0, 8.8);            // 1.2 s dark, straight through it
        s.fix = true;
        s.leg(10, 0.0, 8.8);
        CHECK(s.n_started == 0);
        CHECK(t.state().phase == Phase::Armed);
    }

    CASE("a lap with a dropout in it counts, but cannot become the best");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.lap();                        // arms
        s.leg(20, 0.0, 0.0);            // a 2.0 s stop: lap one is 32.000
        s.lap();                        // closes lap one
        CHECK(t.state().count == 1);
        CHECK_NEAR(t.state().best_ms, 32000.0, 3.0);

        // Lap two is a second quicker, and loses the fix in the middle of it.
        s.fix = false;
        s.leg(5, -8.8, 0.0);            // 1.0 s dark, out on the far side
        s.leg(5,  8.8, 0.0);
        s.fix = true;
        s.lap();                        // closes lap two at 31.000
        CHECK(s.n_completed == 2);
        CHECK(!s.last_lap_valid);
        CHECK_NEAR(s.last_lap_ms, 31000.0, 3.0);
        CHECK(t.state().count == 2);            // the rider rode it, so it counts
        CHECK_NEAR(t.state().last_ms, 31000.0, 3.0);
        CHECK_NEAR(t.state().best_ms, 32000.0, 3.0);   // but it is not the best
    }

    CASE("a hole in the fix stream is not a segment, even if both ends are valid");
    {
        // Fixes keep their valid flag but two seconds go missing — a stalled
        // task, or a receiver that skipped. The pair either side of the hole
        // spans the whole start straight and must not score.
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.leg(10, 0.0, 8.8);
        s.t_ms += 2000;                 // the hole
        s.y = 100.0;                    // ...during which the rider passed the line
        s.leg(10, 0.0, 8.8);
        CHECK(s.n_started == 0);
        CHECK(t.state().phase == Phase::Armed);
    }

    CASE("arm() drops the lap in progress and keeps the session");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.lap();
        s.lap();
        CHECK(t.state().count == 1);
        t.arm();
        CHECK(t.state().phase == Phase::Armed);
        CHECK(t.state().current_ms == 0);
        CHECK(t.state().count == 1);
        CHECK_NEAR(t.state().best_ms, 30000.0, 3.0);
        CHECK(t.has_line());
    }

    CASE("clear() forgets the line and the session");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.lap();
        s.lap();
        t.clear();
        CHECK(!t.has_line());
        CHECK(t.state().phase == Phase::NoLine);
        CHECK(t.state().count == 0);
        CHECK(t.state().best_ms == 0);
        CHECK(t.state().last_ms == 0);
        s.lap();
        CHECK(s.n_completed == 1);      // the one from before clear()
    }

    CASE("distance to the line is reported for the approach screen");
    {
        Timer t;
        t.set_line(kLine, kNorth);
        Sim s(t);
        s.leg(1, 0.0, 8.8);
        CHECK_NEAR(t.state().dist_to_line_ft, 435.6, 5.0);
        s.leg(10, 0.0, 8.8);
        CHECK_NEAR(t.state().dist_to_line_ft, 347.6, 5.0);
    }

    return check::report("lap");
}
