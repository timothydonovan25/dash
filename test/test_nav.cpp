#include "check.hpp"
#include "core/nav/nav.hpp"
using namespace core::nav;

static State press(State s, Button b, Press p, const Layout& l, uint8_t focus = 0) {
    return step(s, InputEvent{b, p}, l, focus);
}

int main() {
    Layout l;                       // {3, 3, 3, 5, 1}
    State s;

    CASE("A cycles through all five modes and wraps");
    for (int i = 0; i < kModeCount; ++i) {
        CHECK(s.mode == static_cast<Mode>(i));
        s = press(s, Button::A, Press::Short, l);
    }
    CHECK(s.mode == Mode::Cruise);

    CASE("A resets screen and focus");
    s = State{Mode::Diag, 3, 2, true};
    s = press(s, Button::A, Press::Short, l);
    CHECK(s.mode == Mode::Config);
    CHECK(s.screen == 0);
    CHECK(s.focus == 0);
    CHECK(!s.menu_open);

    CASE("long A escapes to the cruise main screen from anywhere");
    s = State{Mode::Config, 0, 4, true};
    s = press(s, Button::A, Press::Long, l);
    CHECK(s.mode == Mode::Cruise);
    CHECK(s.screen == 0);
    CHECK(!s.menu_open);

    CASE("B walks screens forward and wraps within the mode");
    s = State{Mode::Diag, 0, 0, false};          // diag has 5 screens
    for (int i = 0; i < 5; ++i) {
        CHECK(s.screen == i);
        s = press(s, Button::B, Press::Short, l);
    }
    CHECK(s.screen == 0);
    CHECK(s.mode == Mode::Diag);                 // B never changes mode

    CASE("long B walks screens backward and wraps");
    s = State{Mode::Cruise, 0, 0, false};        // cruise has 3
    s = press(s, Button::B, Press::Long, l);
    CHECK(s.screen == 2);

    CASE("config mode has one screen, so B is a no-op");
    s = State{Mode::Config, 0, 0, false};
    s = press(s, Button::B, Press::Short, l);
    CHECK(s.screen == 0);

    CASE("C and D move focus and wrap");
    s = State{Mode::Config, 0, 0, false};
    s = press(s, Button::D, Press::Short, l, 4);   CHECK(s.focus == 1);
    s = press(s, Button::D, Press::Short, l, 4);   CHECK(s.focus == 2);
    s = press(s, Button::C, Press::Short, l, 4);   CHECK(s.focus == 1);
    s = press(s, Button::C, Press::Short, l, 4);   CHECK(s.focus == 0);
    s = press(s, Button::C, Press::Short, l, 4);   CHECK(s.focus == 3);  // wraps
    s = press(s, Button::D, Press::Short, l, 4);   CHECK(s.focus == 0);  // wraps

    CASE("C and D do nothing on a screen with no focusable rows");
    s = State{Mode::Cruise, 0, 0, false};
    s = press(s, Button::C, Press::Short, l, 0);   CHECK(s.focus == 0);
    s = press(s, Button::D, Press::Short, l, 0);   CHECK(s.focus == 0);

    CASE("focus is clamped when a screen shrinks under it");
    s = State{Mode::Config, 0, 7, false};
    s = press(s, Button::E, Press::Short, l, 3);
    CHECK(s.focus == 2);

    CASE("short E is left to the screen, long E toggles the menu");
    s = State{Mode::Track, 1, 2, false};
    State before = s;
    s = press(s, Button::E, Press::Short, l, 4);
    CHECK(s == before);
    s = press(s, Button::E, Press::Long, l, 4);
    CHECK(s.menu_open);
    CHECK(s.focus == 0);
    s = press(s, Button::E, Press::Long, l, 4);
    CHECK(!s.menu_open);

    CASE("no button sequence can ever produce an out-of-range screen");
    s = State{};
    const Button seq[] = {Button::A, Button::B, Button::B, Button::A,
                          Button::C, Button::D, Button::E, Button::B};
    for (int i = 0; i < 400; ++i) {
        const Button b = seq[i % 8];
        const Press  p = (i % 7 == 0) ? Press::Long : Press::Short;
        s = press(s, b, p, l, static_cast<uint8_t>(i % 5));
        CHECK(static_cast<uint8_t>(s.mode) < kModeCount);
        CHECK(s.screen < l.screens[static_cast<uint8_t>(s.mode)]);
    }

    return check::report("nav");
}
