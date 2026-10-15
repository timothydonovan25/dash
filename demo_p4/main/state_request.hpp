// `state` on the serial console: one line saying where the UI is, so a
// script driving the board with `btn`/`long` can check it got where it
// meant to instead of counting presses blind. The serial task sets the
// flag; the render task, which owns ui::App, answers it on its next frame.
#pragma once
#include <atomic>

namespace demo {

struct StateRequest {
    std::atomic<bool> pending{false};
};

}  // namespace demo
