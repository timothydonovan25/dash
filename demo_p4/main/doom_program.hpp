// DIAG -> Doom on the demo board (doom/doom.hpp, shared with the bike): its
// 320x200 at 2x on the left of the 800x480 view, and a legend down the
// right edge, over the touch zones that are its buttons.
#pragma once
#include "doom/doom.hpp"

#include <atomic>
#include <cstdint>

namespace demo {

class P4Panel;

// The five buttons as held right now, bit 0 = A: touch zones and the pod
// OR'd together. One writer, the touch task, at 100 Hz.
struct RawButtons {
    std::atomic<uint8_t> mask{0};
};

class DoomBoard final : public doom::Board {
public:
    DoomBoard(P4Panel& panel, const RawButtons& buttons) : panel_(panel), buttons_(buttons) {}

    void begin() override;
    void frame(const uint16_t* rgb565) override;
    uint8_t held() const override { return buttons_.mask.load(); }
    // The demo's bike is a simulation that never stops, so always: here
    // only the bike board keeps Doom to parked.
    bool parked() const override { return true; }

private:
    P4Panel&          panel_;
    const RawButtons& buttons_;
};

}  // namespace demo
