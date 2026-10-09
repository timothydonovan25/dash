// One tick of the `logic` task, with no FreeRTOS in it.
//
// The task itself is now a loop and a delay; everything it decides is here.
// That split exists so the host simulator runs *this* code rather than a
// copy of it. A simulator that mirrors the firmware instead of executing it
// drifts, usually silently, and then stops being evidence of anything.
//
// Nothing in this header may include a FreeRTOS or ESP header, for the same
// reason `core/` may not.
#pragma once
#include "core/fusion/speed.hpp"
#include "services/services.hpp"
#include "core/delta/delta.hpp"
#include "core/geo/geo.hpp"
#include "core/lap/lap.hpp"
#include "core/trip/trip.hpp"

namespace services {

// Where commands come from. The firmware's is a FreeRTOS queue; the
// simulator's is a deque. `logic_tick` only needs to drain one.
class ICommandSource {
public:
    virtual ~ICommandSource() = default;
    virtual bool take(core::state::Command& out) = 0;
};

// Everything the logic task remembers between ticks. Far too big for a task
// stack, so the caller owns it.
struct LogicState {
    static constexpr size_t kMaxLaps = 64;

    core::lap::Timer      lap;
    core::trip::Computer  trip;
    core::trip::FuelModel fuel;
    core::delta::Config   delta_cfg;

    uint32_t laps_ms[kMaxLaps] = {};
    size_t   lap_count = 0;

    core::geo::LatLon origin{};
    bool     have_origin = false;
    uint32_t last_trace_ms = 0;

    // Which ticks get a blackbox row. The rate is a setting, so this is not
    // simply every tick.
    core::log::Decimator log_gate;

    // Stage 1 of fusion: road speed (core/fusion).
    core::fusion::SpeedFilter speed;
    core::fusion::WheelCheck  wheels;
    uint32_t last_gps_utc_ms = 0;
};

// Call at 50 Hz. Drains commands, advances the trip computer and the lap
// timer, feeds the delta matcher and the traces, and publishes the Trip and
// Lap slots.
void logic_tick(Deps& d, LogicState& s, ICommandSource& commands, uint32_t now_ms);

// Restore the odometer and engine hours from NVS. Called once, before the
// first tick.
void logic_restore(Deps& d, LogicState& s);

}  // namespace services
