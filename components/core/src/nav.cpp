#include "core/nav/nav.hpp"

namespace core::nav {

State step(State s, InputEvent e, const Layout& layout, uint8_t focus_count) {
    const uint8_t mode_idx = static_cast<uint8_t>(s.mode);

    switch (e.button) {
    case Button::A:
        // Mode change always resets position. Long press is a panic exit
        // straight back to the main cruise screen — the one you want when
        // something ahead of you needs attention. Short A comes on the
        // press and long A after it, on the same hold
        // (core::input::Config::press_long_mask), so the escape lands
        // wherever the short one went.
        if (e.press == Press::Long) {
            s.mode = Mode::Cruise;
        } else {
            s.mode = static_cast<Mode>((mode_idx + 1) % (layout.mode_count ? layout.mode_count : 1));
        }
        s.screen = 0;
        s.focus = 0;
        s.menu_open = false;
        break;

    case Button::B: {
        // Forward only, on the press (core::input::Config::on_press_mask).
        // Held, it does nothing more: long B was "previous screen", and
        // knowing whether a press would become one meant every screen flip
        // waited for the thumb to come up.
        if (e.press == Press::Long) break;
        const uint8_t n = layout.screens[static_cast<uint8_t>(s.mode)];
        if (n == 0) break;
        s.screen = static_cast<uint8_t>((s.screen + 1) % n);
        s.focus = 0;
        break;
    }

    case Button::C:
        if (focus_count == 0) break;
        s.focus = (s.focus == 0) ? static_cast<uint8_t>(focus_count - 1)
                                 : static_cast<uint8_t>(s.focus - 1);
        break;

    case Button::D:
        if (focus_count == 0) break;
        s.focus = static_cast<uint8_t>((s.focus + 1) % focus_count);
        break;

    case Button::E:
        // Short E is "accept" and belongs to the screen, not to nav, so
        // nav leaves the state alone. Long E toggles the overlay menu.
        if (e.press == Press::Long) {
            s.menu_open = !s.menu_open;
            s.focus = 0;
        }
        break;
    }

    // Never leave focus pointing past the end of the current screen.
    if (focus_count == 0) s.focus = 0;
    else if (s.focus >= focus_count) s.focus = static_cast<uint8_t>(focus_count - 1);

    return s;
}

const char* mode_name(Mode m) {
    switch (m) {
    case Mode::Cruise: return "CRUISE";
    case Mode::Track:  return "TRACK";
    case Mode::Stat:   return "STAT";
    case Mode::Diag:   return "DIAG";
    case Mode::Config: return "CONFIG";
    default:           return "?";
    }
}

}  // namespace core::nav
