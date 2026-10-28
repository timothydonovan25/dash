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
#include "core/lap/sectors.hpp"
#include "core/fixtime/fixtime.hpp"
#include "core/hr/hr.hpp"
#include "core/perf/perf.hpp"
#include "core/session/session.hpp"
#include "core/stunt/stunt.hpp"
#include "core/track/track.hpp"
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
    // Set line presses refused for want of speed (Lap::line_refused_seq).
    uint16_t line_refused_seq = 0;
    bool     lap_told_stale = false;
    // When each fix was taken, on the system clock, from its own GPS time
    // (core::fixtime) rather than from when it arrived; and the last one.
    core::fixtime::Clock fix_clock;
    uint32_t last_fix_ms = 0;

    // Splits and the ideal lap, fed the same fixes as `lap`; the last split
    // crossed, kept for the Lap slot until the next one.
    core::lap::Sectors sectors;
    core::lap::Split   last_split;
    // Peaks: this lap's, into LapHistory when it ends, and the session's
    // lean each way, until IMU's short E resets them.
    core::lap::Peaks   lap_peaks;
    core::lap::Peaks   session_peaks;
    // The last lap's end, and whether it was a new best: LAP flashes it.
    uint32_t last_done_ms = 0;
    bool     last_was_best = false;

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

    // Saved tracks (core::track, services::TrackMailbox). The library
    // itself is the mailbox's `lib`, in PSRAM: it is `logic`'s once the
    // List has been answered.
    bool     tracks_asked = false;     // the List has been posted
    bool     tracks_known = false;     // and answered
    int8_t   track = -1;               // the saved track the line is, -1 none
    bool     track_recalled = false;   // it came back by itself
    bool     track_unsaved = false;    // a line set before the library came
    bool     track_dirty = false;      // its record has news for the card
    bool     want_ref = false;         // its reference lap is to be fetched
    int8_t   forget_slot = -1;         // a file to delete
    // LapClear or TrackForget: the rider took the line away, so no track
    // comes back by itself until the next power-on.
    bool     recall_off = false;
    uint32_t retry_at_ms = 0;          // a failed load or save waits for this
    uint8_t  load_tries = 0;
    // Where a line set before the library came is, until it can be saved.
    core::track::Line pending_line{};
    uint32_t pending_day = 0;
    // The lap time of the delta's reference, 0 for none: the session best,
    // or the track's saved best until the session beats it.
    uint32_t ref_ms = 0;

    // The ride so far, for TRACK -> SESSION.
    core::session::Tracker session;

    // Performance timers off the fused speed, and wheelies and stoppies
    // scored (STAT -> PERF and STUNT, and the toast).
    core::perf::Timer   perf;
    core::stunt::Scorer stunt;
    // Heart-rate recovery when parked after a ride (STAT -> HEART).
    core::hr::Recovery  hr_recovery;

    // Gear calibration: the guided run, and the learner that runs the rest
    // of the time (core/gearcal). `logic` is the one writer of both and of
    // the table entries they change.
    core::gearcal::Guide   guide;
    core::gearcal::Learner learner;

    // The detail log (core/log/detail.hpp): what changed since the last
    // pass, and the scratch the events and the 50 Hz row are built in, here
    // rather than on logic's stack.
    core::log::EventWatcher events;
    core::log::Watched      watched;
    core::log::EventRow     event_out[core::log::EventWatcher::kMaxPerPass];
    core::log::DetRow       det;
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
