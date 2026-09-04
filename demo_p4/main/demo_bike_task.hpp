// Stands in for ctrl/imu/gnss/logic on the real dash: there is no bike here,
// so this task drives sim::Bike (sim/sim_bike.hpp) round its built-in
// circuit and calls services::logic_tick (components/services/include/
// services/logic_tick.hpp) at 50 Hz — the same two pieces sim/sim_main.cpp
// uses to make the desktop simulator run the firmware's own logic instead
// of a copy of it. The only thing new here is that the numbers animate on
// their own instead of coming from sliders, since this is a demo running
// unattended.
#pragma once
#include "services/services.hpp"

namespace demo {

void start_demo_bike(services::Deps& d, services::CommandQueue& q);

}  // namespace demo
