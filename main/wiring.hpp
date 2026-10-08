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

// The rest of the boot-critical hardware: the panel, the IMU and the pod.
// Call once, after build(), before starting render, imu or input. The GPS
// and the card are brought up later by their own tasks.
void finish();

services::CommandQueue& commands();

}  // namespace wiring
