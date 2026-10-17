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
#include "core/gearcal/gearcal.hpp"
#include "services/services.hpp"
#include "core/delta/delta.hpp"
#include "core/geo/geo.hpp"
#include "core/lap/lap.hpp"
#include "core/trip/trip.hpp"
#include "core/waypoint/waypoint.hpp"

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
    uint32_t last_trace_ms = 0;     // rx_ms of the fix behind the last trace point

    // The last fix handed to the lap timer (Gnss::rx_ms), and whether it has
    // already been told the receiver went quiet.
    uint32_t last_lap_rx_ms = 0;
    bool     lap_told_stale = false;

    // The named waypoints (core::waypoint), and whether START has been
    // looked at yet this power-on.
    core::state::Waypoints waypoints;
    bool start_checked = false;

    // Which ticks get a blackbox row. The rate is a setting, so this is not
    // simply every tick.
    core::log::Decimator log_gate;

    // Stage 1 of fusion: road speed (core/fusion).
    core::fusion::SpeedFilter speed;
    core::fusion::WheelCheck  wheels;
    uint32_t last_gps_utc_ms = 0;

    // Gear calibration: the guided run, and the learner that runs the rest
    // of the time (core/gearcal). `logic` is the one writer of both and of
    // the table entries they change.
    core::gearcal::Guide   guide;
    core::gearcal::Learner learner;
};

// Why this pass is running. `logic` ticks on a fixed 50 Hz schedule and is
// also woken between ticks when a producer has something for it (a new fix,
// a command), so the rider sees it without waiting for the schedule.
//
// Everything that integrates over time (trip, fusion, the lap clock) takes
// the real `now_ms` and does not care which kind of pass it is. What needs an
// even cadence runs on Scheduled passes only: blackbox rows, whose rates are
// divisors of the 50 Hz tick (core/log), and the gear calibration's
// windows, which average samples and must not weight the moments a fix
// arrived over the rest.
enum class LogicPass : uint8_t { Scheduled, Event };

// Drains commands, advances the trip computer and the lap timer, feeds the
// delta matcher and the traces, and publishes the Trip and Lap slots. Call
// with Scheduled at 50 Hz, and with Event as often as there is news.
void logic_tick(Deps& d, LogicState& s, ICommandSource& commands, uint32_t now_ms,
                LogicPass pass = LogicPass::Scheduled);

// Restore the odometer and engine hours from NVS. Called once, before the
// first tick.
void logic_restore(Deps& d, LogicState& s);

}  // namespace services
