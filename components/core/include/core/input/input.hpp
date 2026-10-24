// Buttons: debounce, and short versus long versus held.
//
// This is logic, not hardware, so it lives here rather than in
// `input_service` — the whole point of rule 3 is that "does a 700 ms press
// produce one long event or four short ones" is a test, not a ride.
//
// The pod is polled at 200 Hz because that connector has no interrupt line.
// Every window below is in milliseconds rather than in polls, so the rate
// can change without silently changing the feel of the buttons.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/nav/nav.hpp"

namespace core::input {

inline constexpr uint8_t kButtonCount = 5;   // A B C D E

// Which MCP23017 port-A pin each button is wired to, measured on the pod
// itself (2026-10-11): A=GPA3, B=GPA5, C=GPA7, D=GPA4, E=GPA6. GPA0..2 are
// not connected. Index is the button, bit 0 = A.
inline constexpr uint8_t kPodPin[kButtonCount] = {3, 5, 7, 4, 6};

// GPIOA as read from the pod (pressed = 1 after IPOL) to the button mask the
// Decoder takes: bit 0 = A, set means pressed.
constexpr uint8_t pod_pins_to_buttons(uint8_t gpa) {
    uint8_t m = 0;
    for (uint8_t b = 0; b < kButtonCount; ++b)
        if (gpa & (1u << kPodPin[b])) m |= static_cast<uint8_t>(1u << b);
    return m;
}

struct Config {
    // A press must read the same for this long before it counts. Bar-mounted
    // switches on a vibrating dirt bike bounce for a few milliseconds.
    //
    // Two polls at 200 Hz (bsp::kPodPollHz). It stays a stability window
    // rather than becoming a leading-edge lockout: this is half a metre of
    // unshielded I2C running past an ignition coil, and a pin pulled low by
    // noise at the instant of a read is a real failure here, not a
    // theoretical one. Requiring the reading twice costs one poll and
    // removes that whole class.
    //
    // The window has to be shorter than one poll period, or it is three
    // reads, not two: at 100 Hz 12 ms was t, t+10 still short, t+20. 4 makes
    // the second read, 5 ms after the edge, the one that settles it, with
    // room for a poll that lands a millisecond early. A spike seen by one
    // read alone is still never a press. Was 8 at 100 Hz: a press is now
    // seen 5-10 ms after it lands rather than 10-20.
    uint16_t debounce_ms = 4;

    // Held for this long is a long press, and it fires there rather than on
    // release: a rider holding E to set the finish line wants to know it
    // took without letting go first.
    uint16_t long_press_ms = 600;

    // C and D repeat while held, so a config value can be run up without
    // pressing forty times.
    uint16_t repeat_delay_ms = 500;
    uint16_t repeat_ms       = 120;
    uint8_t  repeat_mask     = (1u << 2) | (1u << 3);   // C and D
    // Buttons that act the moment they settle down, not when they come up.
    // A button with a long-press meaning has to wait for the release to
    // know which press it was; one without has nothing to wait for, and
    // waiting cost the length of the press itself -- 80 to 150 ms with a
    // gloved thumb -- on every cursor move, and on every screen flip. B, C
    // and D have no long meaning anywhere (nav, and every screen's
    // handle()): B used to step back a screen when held, and gave that up
    // so the most-used button would not lag. A button given a long meaning
    // must come out of this mask.
    uint8_t  on_press_mask   = (1u << 1) | (1u << 2) | (1u << 3);   // B, C, D
    // Buttons that act on the press and, held on to long_press_ms, say Long
    // as well, after it. Only for a button whose long meaning is fine to
    // arrive on top of the short one having already happened: long A goes
    // to CRUISE wherever short A took the rider, so the next mode shows for
    // the hold and the escape still lands. Not E: its short and long are
    // different acts (arm a lap or set the line), and doing both is wrong.
    uint8_t  press_long_mask = (1u << 0);                            // A
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
        // The release reports nothing: a long press, a repeat, or an
        // on-press button has already said what this press was.
        bool     release_silent = false;
        // Acted on the press, and a Long is still due if it is held
        // (Config::press_long_mask).
        bool     long_due = false;
        uint32_t edge_ms   = 0;       // when raw last changed
        uint32_t down_ms   = 0;       // when stable went down
        uint32_t repeat_ms = 0;       // when the next repeat is due
    };
    Btn b_[kButtonCount];
};

}  // namespace core::input
