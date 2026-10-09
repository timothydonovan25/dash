// Mode and screen navigation. Pure logic — no hardware, no timing.
//
// Button A cycles mode. Button B cycles screens within a mode. C and D
// move the focus. E accepts, and long-E toggles the overlay menu.
#pragma once
#include <cstdint>

namespace core::nav {

enum class Mode : uint8_t { Cruise = 0, Track, Stat, Diag, Config, Count };
inline constexpr uint8_t kModeCount = static_cast<uint8_t>(Mode::Count);

enum class Button : uint8_t { A = 0, B, C, D, E };
enum class Press  : uint8_t { Short = 0, Long };

struct InputEvent {
    Button button;
    Press  press;
};

// How many screens each mode owns. Config always has exactly one: the
// generated ConfigItem list.
struct Layout {
    uint8_t screens[kModeCount] = {6, 3, 3, 6, 1};
};

struct State {
    Mode    mode      = Mode::Cruise;
    uint8_t screen    = 0;
    uint8_t focus     = 0;
    bool    menu_open = false;

    bool operator==(const State& o) const {
        return mode == o.mode && screen == o.screen &&
               focus == o.focus && menu_open == o.menu_open;
    }
};

// `focus_count` is how many focusable rows the CURRENT screen has right
// now. Pass 0 for a screen with nothing to focus; C and D then do nothing.
State step(State s, InputEvent e, const Layout& layout, uint8_t focus_count);

const char* mode_name(Mode m);

}  // namespace core::nav
