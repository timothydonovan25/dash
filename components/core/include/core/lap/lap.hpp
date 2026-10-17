// Lap timer state machine. Pure logic — time arrives as a parameter.
//
// This is the owner of the jitter problem that `core/geo` deliberately does
// not solve. `crosses_line()` answers a geometry question honestly: a parked
// bike whose fix wanders four feet north really did move north across the
// line. Turning that into "did the rider complete a lap" needs three gates,
// and they live here:
//
//   1. a minimum speed, so the paddock cannot score laps;
//   2. a minimum lap time, so weaving over the line, or one fix pair that
//      straddles it twice, cannot score two;
//   3. a maximum fix gap, so a stale previous position never pairs with a
//      fresh one and teleports across the line.
//
// See the CONTRACT case in test_geo.cpp.
#pragma once
#include <cstdint>
#include "core/geo/geo.hpp"

namespace core::lap {

enum class Phase : uint8_t {
    NoLine = 0,   // no finish line has been set; nothing to do
    Armed,        // line set, waiting for the crossing that starts the session
    Timing,       // a lap is running
};

struct Config {
    // Half width of the finish line. 50 ft each side covers a start straight
    // without reaching the next one back.
    double   half_width_ft  = 50.0;

    // A crossing below this is not a lap. 10 mph at 10 Hz is 1.5 ft per fix,
    // comfortably above both GPS wander and the 0.5 ft guard inside
    // `crosses_line()`, and nobody laps a circuit at walking pace.
    //
    // Feed `Fix::speed_mph` from the receiver's own speed, which comes from
    // Doppler and reads near zero when parked. A speed differenced from
    // successive positions does not — it wanders with them, which is the
    // failure this gate exists to stop.
    float    min_speed_mph  = 10.0f;

    // Two crossings closer together than this are one crossing seen twice.
    uint32_t min_lap_ms     = 10000;

    // Longest gap between two fixes that may still be treated as one
    // continuous segment. The GPS runs at 10 Hz and `logic` at 50 Hz, so
    // anything past 1.5 s means fixes were dropped and the position between
    // them is unknown.
    uint32_t max_fix_gap_ms = 1500;
};

struct Fix {
    geo::LatLon pos{};
    float       speed_mph = 0.0f;
    bool        valid     = false;   // receiver has a fix
    uint32_t    t_ms      = 0;       // when this fix was sampled
};

// What goes in the `Lap` publish slot. Milliseconds, because that is what a
// lap time is read in; 0 for `best_ms` means "no lap yet".
struct State {
    Phase    phase           = Phase::NoLine;
    uint16_t count           = 0;    // laps completed this session
    uint32_t current_ms      = 0;
    uint32_t last_ms         = 0;
    uint32_t best_ms         = 0;
    float    dist_to_line_ft = 0.0f; // straight line, not along the track
};

enum class Event : uint8_t {
    None = 0,
    Started,      // first crossing; the clock is now running
    Completed,    // a lap was scored
};

struct Update {
    Event    event  = Event::None;
    uint32_t lap_ms = 0;      // meaningful when event == Completed
    // False when the fix dropped out somewhere inside this lap. The lap is
    // still shown and still counted — the rider rode it — but it may be two
    // laps glued together by a crossing nobody saw, so it is not allowed to
    // become the session best.
    bool     valid  = true;
    // The instant of the crossing, for Started and Completed: when the lap
    // began, interpolated inside the fix pair. Sectors start from it.
    uint32_t at_ms   = 0;
    float    at_frac = 0.0f;
};

class Timer {
public:
    // From the LapSetLine command: the rider's position and course as they
    // sit on, or ride over, the start/finish line. Direction matters —
    // crossing the other way does not count.
    void set_line(geo::LatLon at, double heading_deg);

    // LapArm. Throws away the lap in progress and waits for the line again.
    // Counts and the session best survive.
    void arm();

    // LapClear. Forgets the line and the whole session.
    void clear();

    // Call once per new fix, with the time the fix was taken, and with
    // `valid` false when the receiver has none. Not once per `logic` tick:
    // see advance().
    Update update(const Fix& f, const Config& c);

    // Moves the running lap clock to `now_ms` between fixes, so it counts
    // at the caller's rate rather than the receiver's. Call update() once
    // per *new* fix only: feeding it the same fix again makes the next
    // segment look 20 ms long instead of 100, and every crossing is then
    // interpolated over the wrong interval.
    void advance(uint32_t now_ms);

    const State& state() const { return pub_; }
    bool has_line() const { return has_line_; }

private:
    // A moment between two fixes. `frac` is the sub-millisecond remainder, so
    // that interpolating the crossing survives the trip through integer ms.
    // At 60 mph and 10 Hz a fix pair spans 8.8 ft, so which end of it the
    // line fell nearer is worth about 100 ms of lap time.
    struct Instant {
        uint32_t ms   = 0;
        float    frac = 0.0f;
    };

    void begin_lap(Instant at);

    geo::LatLon line_{};
    double      line_heading_deg_ = 0.0;
    bool        has_line_ = false;

    geo::LatLon prev_{};
    uint32_t    prev_t_ms_ = 0;
    bool        has_prev_  = false;

    Instant     lap_start_{};
    bool        gapped_ = false;      // fix lost since this lap began

    State       pub_{};
};

}  // namespace core::lap
