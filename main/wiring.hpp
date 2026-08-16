// Construction, separated from the boot sequence.
//
// `wiring.cpp` owns every long-lived object and hands out references;
// `main.cpp` decides what order things happen in and starts the tasks. Two
// files because they answer two different questions, and the second one is
// the one you read when the boot order is wrong.
#pragma once
#include "services/services.hpp"

namespace wiring {

// Constructs everything, brings the hardware up in the order §6 requires,
// and returns the dependency set the tasks take. Call once.
services::Deps& build();

services::CommandQueue& commands();

}  // namespace wiring
