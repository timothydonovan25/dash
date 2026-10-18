// `state` on the serial console: one line saying where the UI is, so a
// script driving the board with `btn`/`long` can check it got where it
// meant to instead of counting presses blind. The serial task sets the
// flag; the render task, which owns ui::App, answers it on its next frame.
#pragma once
#include <atomic>
#include <cstdint>

namespace demo {

struct StateRequest {
    std::atomic<bool> pending{false};
    // `inputs`: the touch task logs every change of the raw pod pins and
    // touch zones, and every press the Decoder makes of them, until this
    // millisecond. For a button that misses presses: it says whether the
    // switch, the touch panel or the decoding dropped it.
    std::atomic<uint32_t> trace_inputs_until_ms{0};
};

}  // namespace demo
