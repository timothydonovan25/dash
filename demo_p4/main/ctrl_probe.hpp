#pragma once
#include "services/services.hpp"

namespace demo {

// A stand-in for `ctrl`'s timing, with none of its hardware: a 500 Hz task
// on core 0 at ctrl's priority, measuring how late each tick wakes while
// render, BLE and the C6 link run on real silicon. See ctrl_probe.cpp.
void start_ctrl_probe(services::Deps& d);

}  // namespace demo
