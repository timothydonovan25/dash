// Buttons: debounce, and short versus long versus held.
//
// This is logic, not hardware, so it lives here rather than in
// `input_service` — the whole point of rule 3 is that "does a 700 ms press
// produce one long event or four short ones" is a test, not a ride.
//
// The pod is polled at 100 Hz because that connector has no interrupt line.
// Every window below is in milliseconds rather than in polls, so the rate
// can change without silently changing the feel of the buttons.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/nav/nav.hpp"

namespace core::input {

inline constexpr uint8_t kButtonCount = 5;   // A B C D E

struct Config {
    // A press must read the same for this long before it counts. Bar-mounted
    // switches on a vibrating dirt bike bounce for a few milliseconds.
    //
    // Two polls at 100 Hz. It stays a stability window rather than becoming
    // a leading-edge lockout, which would be about 20 ms quicker: this is
    // half a metre of unshielded I2C running past an ignition coil, and a
    // pin pulled low by noise at the instant of a read is a real failure
    // here, not a theoretical one. Requiring the reading twice costs one
    // poll and removes that whole class.
    //
    // 25 ms was simply more conservative than a switch needs.
    uint16_t debounce_ms = 12;

    // Held for this long is a long press, and it fires there rather than on
    // release: a rider holding E to set the finish line wants to know it
    // took without letting go first.
    uint16_t long_press_ms = 600;

    // C and D repeat while held, so a config value can be run up without
    // pressing forty times.
    uint16_t repeat_delay_ms = 500;
    uint16_t repeat_ms       = 120;
    uint8_t  repeat_mask     = (1u << 2) | (1u << 3);   // C and D
};

class Decoder {
public:
    // Feed the raw pod mask, bit 0 = A, set means pressed. Writes the events
    // that resulted into `out` and returns how many. At most one event per
    // button per poll, so `max` of kButtonCount is always enough.
    size_t update(uint8_t raw_mask, uint32_t t_ms, const Config& c,
                  nav::InputEvent* out, size_t max);

    // True while the button is down and settled. The render task uses this
    // for the "hold to confirm" bar.
    bool held(nav::Button b) const;
    uint32_t held_ms(nav::Button b, uint32_t t_ms) const;

    void reset();

private:
    struct Btn {
        bool     stable    = false;   // debounced state
        bool     raw       = false;   // last raw reading
        bool     long_sent = false;
        uint32_t edge_ms   = 0;       // when raw last changed
        uint32_t down_ms   = 0;       // when stable went down
        uint32_t repeat_ms = 0;       // when the next repeat is due
    };
    Btn b_[kButtonCount];
};

}  // namespace core::input
