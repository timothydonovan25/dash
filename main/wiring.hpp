// Construction, separated from the boot sequence.
//
// `wiring.cpp` owns every long-lived object and hands out references;
// `main.cpp` decides what order things happen in and starts the tasks. Two
// files because they answer two different questions, and the second one is
// the one you read when the boot order is wrong.
#pragma once
#include "services/services.hpp"

namespace wiring {

// Constructs everything and brings up only what `ctrl` needs -- the LED
// driver, the tach counters, VIN -- so the shift lamps can start before the
// panel is awake. Returns the dependency set the tasks take. Call once.
services::Deps& build();

// The panel. Call once, after build(), before starting render.
void finish_panel();

// The IMU and the pod. Call once, after finish_panel(), before starting imu
// or input. After render has started, so the first frame does not wait on
// them. The GPS and the card are brought up later by their own tasks.
void finish_sensors();

services::CommandQueue& commands();

}  // namespace wiring
