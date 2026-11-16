// Mode and screen navigation. Pure logic — no hardware, no timing.
//
// Button A cycles mode. Button B cycles screens within a mode. C and D
// move the focus. E accepts, and long-E toggles the overlay menu.
#pragma once
#include <cstdint>

namespace core::nav {

// The stock layout's modes. With a custom layout (core::layout::Deck) the
// riding modes are whatever the rider made, still numbered from 0, and DIAG
// and CONFIG are always the last two: use is_diag() / is_config(), never
// compare with Mode::Diag or Mode::Config, outside the stock layout's tests.
enum class Mode : uint8_t { Cruise = 0, Track, Stat, Diag, Config, Count };
inline constexpr uint8_t kModeCount = static_cast<uint8_t>(Mode::Count);
// Up to 8 riding modes, plus DIAG and CONFIG.
inline constexpr uint8_t kMaxModes = 10;

enum class Button : uint8_t { A = 0, B, C, D, E };
enum class Press  : uint8_t { Short = 0, Long };

struct InputEvent {
    Button button;
    Press  press;
};

// How many screens each mode owns. Config always has exactly one: the
// generated ConfigItem list.
struct Layout {
    uint8_t mode_count = kModeCount;
    // DIAG's are its menu and eight pages, reached from the menu rather than
    // walked with B (ui::App).
    uint8_t screens[kMaxModes] = {7, 5, 6, 9, 1};
};
inline constexpr uint8_t kDiagScreens = 9;

inline bool is_diag(const Layout& l, Mode m) {
    return static_cast<uint8_t>(m) + 2 == l.mode_count;
}
inline bool is_config(const Layout& l, Mode m) {
    return static_cast<uint8_t>(m) + 1 == l.mode_count;
}

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
