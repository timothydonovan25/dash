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
#include "core/crash/crash.hpp"
#include "core/delta/delta.hpp"
#include "core/geo/geo.hpp"
#include "core/lap/lap.hpp"
#include "core/lap/sectors.hpp"
#include "core/fixtime/fixtime.hpp"
#include "core/hr/hr.hpp"
#include "core/perf/perf.hpp"
#include "core/route/navigator.hpp"
#include "core/route/pace.hpp"
#include "core/session/session.hpp"
#include "core/slip/slip.hpp"
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

    // This tick's copy of the gnss slot. Here, not on logic_tick's stack,
    // which is at the 1 kB frame limit.
    core::state::Gnss     gnss_now;
    core::lap::Timer      lap;
    core::trip::Computer  trip;
    // The blackbox's SlipPct column; render's own meter draws the bar.
    core::slip::Meter     log_slip;
    // Console `odo` and `hours` have each been given (Deps::totals_unread).
    bool odo_set = false, hours_set = false;
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

    // Turn-by-turn (core::route, services::RouteMailbox). The routes
    // themselves are the mailbox's, in PSRAM; this is the state machine and
    // what the card said last.
    core::route::Navigator nav;
    core::route::Progress  nav_progress;
    core::state::Nav::Label route_labels[core::state::Nav::kMaxLabels];   // label_route's
    uint8_t                n_route_labels = 0;
    core::state::Nav       nav_out;        // publish_nav's staging copy: too big for its frame
    core::route::PaceNote  pace[core::state::Nav::kMaxPace];   // publish_nav's, here not on its stack
    bool     map_asked = false;
    uint32_t map_asked_ms = 0;
    uint32_t map_date = 0;
    bool     dests_asked = false;
    uint32_t dests_asked_ms = 0;
    core::route::Dest dests[RouteMailbox::kMaxDests];
    uint8_t  n_dests = 0;
    // The list as last published: NavGo's index is into this.
    core::state::NavDest nav_list[core::state::Nav::kMaxDest];
    uint8_t  nav_list_n = 0;
    char     nav_target[16] = {};
    bool     route_traced = false;    // RouteTrace holds a route
    // "Where to?" (core/route/search.hpp): what has been typed, and what
    // the card said about it.
    uint8_t  search = core::state::Nav::SearchOff;
    char     query[core::route::kKeyLen + 1] = {};
    bool     search_want = false;         // the query changed: ask the card
    bool     search_ok = false;
    bool     searching = false;
    uint64_t search_next = 0;
    uint32_t search_matches = 0;
    core::route::SearchHit hits[RouteMailbox::kMaxHits];
    uint8_t  n_hits = 0;
    core::route::SearchHit picked;        // the street a number is for
    char     digits[8] = {};
    bool     house_want = false;
    uint16_t usb_refused_seq = 0;         // USB drive asked for while moving
    // A place found and gone to: to the card's recent list (route/recent.csv).
    core::route::Dest recent{};
    bool     recent_want = false;
    uint32_t phone_seq = 0;           // the last PhoneDest acted on
    core::state::NavDest phone_dest;  // and where it was, for the picker
    bool     have_phone_dest = false;

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
    // A crash, its countdown and the SOS (core::crash).
    core::crash::Detector crash;

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
    // What the passes since the last row saw, for that row: a fix wakes an
    // Event pass, and that pass is the only one where GPS is new and used.
    bool    det_gps_new = false;
    uint8_t det_used = 0;
    uint8_t det_rejected = 0;
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
