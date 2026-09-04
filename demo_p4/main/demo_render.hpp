// The demo's render loop — the same job components/services/src/
// render_service.cpp does for the real dash, simplified because this board
// doesn't have the ILI9488's SPI bottleneck (components/services/src/
// render_service.cpp's comment block explains why the real one bands and
// diffs: 92ms for a whole frame over 4-wire SPI at 40MHz). The DPI panel's
// framebuffer is written directly and scanned out by hardware, so there's
// no wire budget to protect — this always repaints the whole 480x320 canvas
// every tick, which keeps the code an order of magnitude smaller than the
// real render task for a build that only ever needs to look good, not run
// forever on a bike.
#pragma once
#include "services/services.hpp"

namespace demo {

// Starts the render task pinned to core 0 — this board has one thing to
// do. Reuses ui::App and core::nav::InputEvent exactly as the real render
// task does; runs until the demo is powered off.
void start_demo_render(services::Deps& d, services::CommandQueue& q);

}  // namespace demo
