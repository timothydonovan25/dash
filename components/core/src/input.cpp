#include "core/input/input.hpp"

namespace core::input {

using nav::Button;
using nav::InputEvent;
using nav::Press;

void Decoder::reset() {
    for (uint8_t i = 0; i < kButtonCount; ++i) b_[i] = Btn{};
}

bool Decoder::held(Button b) const {
    return b_[static_cast<uint8_t>(b)].stable;
}

uint32_t Decoder::held_ms(Button b, uint32_t t_ms) const {
    const Btn& s = b_[static_cast<uint8_t>(b)];
    return s.stable ? (t_ms - s.down_ms) : 0;
}

size_t Decoder::update(uint8_t raw_mask, uint32_t t_ms, const Config& c,
                       InputEvent* out, size_t max) {
    size_t n = 0;

    for (uint8_t i = 0; i < kButtonCount; ++i) {
        Btn& s = b_[i];
        const bool raw = ((raw_mask >> i) & 1u) != 0;

        if (raw != s.raw) {
            s.raw = raw;
            s.edge_ms = t_ms;          // start the settling window again
            continue;
        }
        // Unsigned throughout, so a wrapped millisecond counter reads as a
        // small interval rather than as 49 days.
        if (raw != s.stable && (t_ms - s.edge_ms) >= c.debounce_ms) {
            s.stable = raw;
            if (raw) {
                s.down_ms = t_ms;
                s.release_silent = false;
                s.long_due = false;
                s.repeat_ms = t_ms + c.repeat_delay_ms;
                if ((c.press_long_mask >> i) & 1u) {
                    // Acts now too, and is still listened to for a hold.
                    s.release_silent = true;
                    s.long_due = true;
                    if (n < max) out[n++] = InputEvent{static_cast<Button>(i), Press::Short};
                } else if ((c.on_press_mask >> i) & 1u) {
                    // Acts now, so the release has nothing left to say, and
                    // a button with no long meaning never makes one.
                    s.release_silent = true;
                    if (n < max) out[n++] = InputEvent{static_cast<Button>(i), Press::Short};
                }
            } else if (!s.release_silent) {
                // Released before it became a long press. A long press has
                // already reported itself, so releasing after one is silent
                // rather than a short press on the way out.
                if (n < max) out[n++] = InputEvent{static_cast<Button>(i), Press::Short};
            }
        }

        if (!s.stable) continue;

        const uint32_t down_for = t_ms - s.down_ms;

        if ((!s.release_silent || s.long_due) && down_for >= c.long_press_ms) {
            s.release_silent = true;
            s.long_due = false;
            if (n < max) out[n++] = InputEvent{static_cast<Button>(i), Press::Long};
            continue;
        }

        // Repeat is for the cursor keys only: a repeating "next mode" would
        // make the dash unusable with a gloved thumb.
        const bool repeats = ((c.repeat_mask >> i) & 1u) != 0;
        if (repeats && down_for >= c.repeat_delay_ms &&
            static_cast<int32_t>(t_ms - s.repeat_ms) >= 0) {
            s.repeat_ms = t_ms + c.repeat_ms;
            s.release_silent = true;    // a held cursor key is repeat, not long
            if (n < max) out[n++] = InputEvent{static_cast<Button>(i), Press::Short};
        }
    }

    return n;
}

}  // namespace core::input
