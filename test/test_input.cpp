#include "check.hpp"
#include "core/input/input.hpp"

using namespace core::input;
using core::nav::Button;
using core::nav::InputEvent;
using core::nav::Press;

namespace {
constexpr uint8_t kA = 1u << 0;
constexpr uint8_t kC = 1u << 2;
constexpr uint8_t kE = 1u << 4;

// The pod is polled at 100 Hz because that connector has no interrupt line.
struct Pod {
    Decoder  d;
    Config   c;
    uint32_t t_ms = 0;
    int      shorts[kButtonCount] = {};
    int      longs[kButtonCount] = {};

    void poll(uint8_t mask, uint32_t ms) {
        for (uint32_t i = 0; i < ms; i += 10) {
            t_ms += 10;
            InputEvent ev[kButtonCount];
            const size_t n = d.update(mask, t_ms, c, ev, kButtonCount);
            for (size_t k = 0; k < n; ++k) {
                const uint8_t b = static_cast<uint8_t>(ev[k].button);
                if (ev[k].press == Press::Short) ++shorts[b];
                else ++longs[b];
            }
        }
    }
    int total_events() const {
        int n = 0;
        for (uint8_t i = 0; i < kButtonCount; ++i) n += shorts[i] + longs[i];
        return n;
    }
};
}  // namespace

int main() {
    CASE("pod pins map to buttons as the pod is wired");
    {
        // Measured: A=GPA3, B=GPA5, C=GPA7, D=GPA4, E=GPA6.
        CHECK(pod_pins_to_buttons(1u << 3) == (1u << 0));
        CHECK(pod_pins_to_buttons(1u << 5) == (1u << 1));
        CHECK(pod_pins_to_buttons(1u << 7) == (1u << 2));
        CHECK(pod_pins_to_buttons(1u << 4) == (1u << 3));
        CHECK(pod_pins_to_buttons(1u << 6) == (1u << 4));
        CHECK(pod_pins_to_buttons(0x00) == 0);
        CHECK(pod_pins_to_buttons(0xF8) == 0x1F);
        // The unconnected pins can never press anything.
        CHECK(pod_pins_to_buttons(0x07) == 0);
    }

    CASE("a clean tap is one short press, on release");
    {
        Pod p;
        p.poll(0, 200);
        p.poll(kA, 100);
        CHECK(p.total_events() == 0);          // nothing yet: still held
        p.poll(0, 100);
        CHECK(p.shorts[0] == 1);
        CHECK(p.longs[0] == 0);
    }

    CASE("contact bounce is one press, not five");
    {
        // A bar switch on a vibrating dirt bike chatters for a few ms on the
        // way in and on the way out.
        Pod p;
        p.poll(0, 200);
        for (int i = 0; i < 5; ++i) { p.poll(kE, 10); p.poll(0, 10); }
        p.poll(kE, 200);
        for (int i = 0; i < 5; ++i) { p.poll(0, 10); p.poll(kE, 10); }
        p.poll(0, 200);
        CHECK(p.shorts[4] == 1);
        CHECK(p.longs[4] == 0);
    }

    CASE("a long press fires while the button is still down");
    {
        // Holding E to set the finish line has to confirm itself before the
        // rider lets go, or they have no idea whether it took.
        Pod p;
        p.poll(0, 100);
        p.poll(kE, 500);
        CHECK(p.longs[4] == 0);
        p.poll(kE, 200);                       // now past 600 ms
        CHECK(p.longs[4] == 1);
        p.poll(0, 200);
        CHECK(p.shorts[4] == 0);               // release after a long is silent
        CHECK(p.longs[4] == 1);                // and it only fires once
    }

    CASE("holding a cursor key repeats; holding a mode key does not");
    {
        Pod p;
        p.poll(0, 100);
        p.poll(kC, 2000);
        // 500 ms delay then one every 120 ms: about a dozen in 1.5 s.
        CHECK(p.shorts[2] > 8);
        CHECK(p.shorts[2] < 16);
        CHECK(p.longs[2] == 0);

        Pod q;
        q.poll(0, 100);
        q.poll(kA, 2000);
        CHECK(q.shorts[0] == 0);               // a repeating "next mode" would
        CHECK(q.longs[0] == 1);                //   make the dash unusable
    }

    CASE("two buttons at once are two independent presses");
    {
        Pod p;
        p.poll(0, 100);
        p.poll(kA | kE, 100);
        p.poll(0, 100);
        CHECK(p.shorts[0] == 1);
        CHECK(p.shorts[4] == 1);
    }

    CASE("a press settles on its second read, 10 ms after the edge");
    {
        Decoder d;
        Config c;
        core::nav::InputEvent ev[core::input::kButtonCount];
        CHECK(d.update(0x01, 1000, c, ev, core::input::kButtonCount) == 0);
        CHECK(!d.held(core::nav::Button::A));
        d.update(0x01, 1010, c, ev, core::input::kButtonCount);
        CHECK(d.held(core::nav::Button::A));
        // And the release the same way: a short press out on its second read.
        CHECK(d.update(0x00, 1100, c, ev, core::input::kButtonCount) == 0);
        CHECK(d.update(0x00, 1110, c, ev, core::input::kButtonCount) == 1);
    }

    CASE("a spike shorter than the debounce window is not a press");
    {
        Pod p;
        p.poll(0, 200);
        p.poll(kA, 10);                        // one poll only
        p.poll(0, 300);
        CHECK(p.total_events() == 0);
    }

    CASE("a cursor key acts on the press, while it is still down");
    {
        // C and D have no long meaning, so there is nothing to wait for: the
        // press is reported the moment it settles, not when the thumb lifts.
        Pod p;
        p.poll(0, 200);
        p.poll(kC, 20);                        // settled on its second read
        CHECK(p.shorts[2] == 1);
        p.poll(kC, 150);                       // a gloved tap, still held
        CHECK(p.shorts[2] == 1);
        p.poll(0, 200);
        CHECK(p.shorts[2] == 1);               // the release adds nothing
        CHECK(p.longs[2] == 0);
    }

    CASE("an on-press key still ignores bounce and spikes");
    {
        Pod p;
        p.poll(0, 200);
        p.poll(kC, 10);                        // one read only: a spike
        p.poll(0, 300);
        CHECK(p.total_events() == 0);
        for (int i = 0; i < 5; ++i) { p.poll(kC, 10); p.poll(0, 10); }
        p.poll(kC, 200);
        for (int i = 0; i < 5; ++i) { p.poll(0, 10); p.poll(kC, 10); }
        p.poll(0, 200);
        CHECK(p.shorts[2] == 1);
    }

    CASE("a key with a long meaning still waits for the release");
    {
        Pod p;
        p.poll(0, 200);
        p.poll(kE, 150);
        CHECK(p.shorts[4] == 0);               // could still become a long
        p.poll(0, 100);
        CHECK(p.shorts[4] == 1);

        // And a cursor key taken out of the mask goes back to the release.
        Pod q;
        q.c.on_press_mask = 0;
        q.poll(0, 200);
        q.poll(kC, 150);
        CHECK(q.shorts[2] == 0);
        q.poll(0, 100);
        CHECK(q.shorts[2] == 1);
    }

    CASE("held() and held_ms() track the settled state");
    {
        Pod p;
        p.poll(0, 100);
        CHECK(!p.d.held(Button::E));
        p.poll(kE, 300);
        CHECK(p.d.held(Button::E));
        CHECK(p.d.held_ms(Button::E, p.t_ms) > 200);
        p.poll(0, 100);
        CHECK(!p.d.held(Button::E));
        CHECK(p.d.held_ms(Button::E, p.t_ms) == 0);
    }

    CASE("a wrapped millisecond counter does not fire a long press");
    {
        Decoder d;
        Config c;
        InputEvent ev[kButtonCount];
        const uint32_t near_wrap = 0xFFFFFF00u;
        uint32_t t = near_wrap;
        size_t total = 0;
        for (int i = 0; i < 20; ++i) { t += 10; total += d.update(0, t, c, ev, 5); }
        for (int i = 0; i < 10; ++i) { t += 10; total += d.update(kA, t, c, ev, 5); }
        // ~100 ms of hold across the wrap: not long enough to be a long press.
        CHECK(total == 0);
        for (int i = 0; i < 10; ++i) { t += 10; total += d.update(0, t, c, ev, 5); }
        CHECK(total == 1);
    }

    CASE("reset() forgets a button that was down");
    {
        Pod p;
        p.poll(kE, 300);
        CHECK(p.d.held(Button::E));
        p.d.reset();
        CHECK(!p.d.held(Button::E));
    }

    return check::report("input");
}
