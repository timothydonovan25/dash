// The start-up gauge sweep: rpm, speed and gear swing to full scale and
// back, the way an analog cluster's needles do at key-on.
//
// It is a rewrite of the Frame the screens are about to draw, not something
// each screen knows about, so rule 4 holds: a screen is still a pure
// function of one Frame, and during the sweep that Frame says full scale.
// The sweep never touches a slot, so `ctrl`, the logger and the lap timer
// only ever see the real bike.
#pragma once
#include <cstdint>
#include "core/state/slots.hpp"

namespace core::sweep {

// Where the needles go. Not settings: the rpm peak is the shift-flash rpm
// the rider already chose, and the speed peak only has to look like the
// top of a speedo.
inline constexpr float kPeakMph = 60.0f;

// Above this real speed the sweep stops at once: a dash that powers up
// rolling (a stall restarted on the move) shows the bike, not a show.
inline constexpr float kAbortMph = 1.0f;

struct Config {
    bool     enabled     = true;
    uint32_t duration_ms = 2000;
    float    peak_rpm    = 8800.0f;
    uint8_t  gears       = 5;
};

// 0 at the start, 1 at the halfway point, 0 again at duration_ms, eased on
// both halves so the needle slows into the top and settles at the bottom.
// Negative once the sweep is over, or for a zero duration.
float level(uint32_t elapsed_ms, uint32_t duration_ms);

// Rewrites `f` for `elapsed_ms` into the sweep. Returns false, leaving `f`
// alone, once the sweep is finished, disabled, or the bike is moving --
// and the caller should then stop asking, so a sweep that ended never
// restarts.
bool apply(state::Frame& f, uint32_t elapsed_ms, const Config& c);

// The rpm the shift lamps show at `elapsed_ms` into the sweep, so they climb
// and fall with the tach on the screen. Negative once finished, disabled,
// or moving (`road_mph`), and `ctrl` then goes back to the real rpm for good.
float lamp_rpm(uint32_t elapsed_ms, float road_mph, const Config& c);

}  // namespace core::sweep
