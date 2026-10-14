// The demo's render loop — the same job components/services/src/
// render_service.cpp does for the real dash. There is no 8-bit bus here:
// the DPI panel's framebuffer is written directly and scanned out by
// hardware. But every pixel written is a 1.5x scale and a 90-degree turn
// into PSRAM on the CPU, ~345k destination pixels for the whole screen, so
// it sends what changed the same way the bike does (ui::App::damage()) and
// repaints whole only when the App asks for it. It also stands in for
// `keeper` on the health table, since this board has no keeper.
#pragma once
#include "services/services.hpp"

namespace demo {

// Starts the render task pinned to core 0 — this board has one thing to
// do. Reuses ui::App and core::nav::InputEvent exactly as the real render
// task does; runs until the demo is powered off.
void start_demo_render(services::Deps& d, services::CommandQueue& q);

}  // namespace demo
