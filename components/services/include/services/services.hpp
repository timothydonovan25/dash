// The tasks (§5).
//
// Each service is a loop that reads one thing, hands the numbers to a core
// module that has already been tested on the host, and publishes the result
// into exactly one slot. Nothing here does arithmetic that could have lived
// in `core/` — if a service starts to look clever, the clever part is in the
// wrong file.
//
// | Task    | Core | Prio | Rate      | Owns                                |
// |---------|------|------|-----------|-------------------------------------|
// | ctrl    |  0   |  18  | 500 Hz    | tach, wheel speed, shift lamps      |
// | imu     |  0   |  15  | 200 Hz    | FIFO drain, attitude, jumps         |
// | gnss    |  0   |  12  | UART      | NMEA parse, position                |
// | logic   |  0   |  10  | 50 Hz     | lap timer, trip, command drain      |
// | input   |  0   |   9  | 100 Hz    | pod poll, debounce                  |
// | render  |  1   |   6  | 30 Hz     | snapshot, widgets, panel            |
// | logger  |  1   |   5  | drain     | blackbox ring to SD                 |
// | keeper  |  1   |   3  | 1 Hz      | NVS, health retry, rail sense       |
// | storage |  1   |   3  | 2 Hz      | card mount, hotplug, USB hand-off    |
//
// Core 0 is a real-time island: no radio, no filesystem. Anything added to
// it above priority 18 breaks the 3 ms tach-to-lamp budget.
//
// `ble` is not in this build. It is step 9 of the build order, deliberately
// after the real-time path works, so that if the shift light timing changes
// you know the radio caused it.
#pragma once
#include "hal/hal.hpp"
#include "core/state/slots.hpp"
#include "core/state/track_buffer.hpp"
#include "core/config/config.hpp"
#include "core/health/health.hpp"
#include "core/log/log.hpp"
#include "core/delta/delta.hpp"
#include "ui/screen.hpp"

namespace services {

// Every lap scored this session. Fixed size: more laps than this on one
// tank is not a thing a CRF250F does.
struct LapHistory {
    static constexpr size_t kMax = 64;
    uint32_t ms[kMax] = {};
    size_t   count = 0;
    uint32_t best_ms = 0;
};

// Everything every task needs, wired once in main and passed by reference.
// This struct is why there is no `globals.h` in this repo.
struct Deps {
    hal::IClock*       clock   = nullptr;
    hal::IImu*         imu     = nullptr;
    hal::IGnss*        gnss    = nullptr;
    hal::IExpander*    board   = nullptr;
    hal::IButtonPod*   pod     = nullptr;
    hal::IPanel*       panel   = nullptr;
    hal::IEngineSense* sense   = nullptr;
    hal::IEngine*      engine  = nullptr;      // NullEngine today
    hal::IStorage*     storage = nullptr;
    hal::ICard*        card    = nullptr;

    core::state::Slots*     slots    = nullptr;
    core::config::Settings* settings = nullptr;
    core::health::Tracker*  health   = nullptr;
    ui::SystemInfo*         system   = nullptr;

    // The trip breadcrumb and the track trace. Bulk data, one writer each,
    // never in a slot (§4.4).
    core::state::TrackBuffer<core::state::TrackPoint, 4096>* trip_trace = nullptr;
    core::state::TrackBuffer<core::state::TrackPoint, 4096>* lap_trace  = nullptr;

    // The blackbox. Producers push and never wait; `logger` drains it to the
    // card. 512 records at 50 Hz is ten seconds of slack, which is longer
    // than any SD stall anyone has measured.
    core::log::Ring<core::log::Record, 512>* blackbox = nullptr;

    // Lap delta against the session best. Two point traces, so it is far too
    // big for a task stack and lives here like the others.
    core::delta::Tracker* delta = nullptr;

    // Lap times for the Track history screen. The logic task scores them and
    // the render task draws them, so they live here rather than inside
    // either — there was no path between the two before, and the screen
    // showed "no laps yet" for the whole session.
    LapHistory* lap_history = nullptr;
};

// The one command queue (§4.5). Screens post, `logic` drains, and every
// command that changed anything can be logged in one place.
class CommandQueue final : public ui::ICommandSink {
public:
    bool begin(size_t depth = 16);
    bool post(const core::state::Command& c) override;      // any task
    bool post_from_isr(const core::state::Command& c);
    bool take(core::state::Command& out);                   // logic only
    uint32_t dropped() const { return dropped_; }

private:
    void*    q_ = nullptr;          // QueueHandle_t
    uint32_t dropped_ = 0;
};

// Started by main in this order. Each one pins itself to the core in the
// table above; none of them return.
void start_ctrl(Deps& d);
void start_imu(Deps& d);
void start_gnss(Deps& d);
void start_logic(Deps& d, CommandQueue& q);
void start_input(Deps& d, CommandQueue& q);
void start_render(Deps& d, CommandQueue& q);
void start_keeper(Deps& d);
void start_logger(Deps& d);
void start_storage(Deps& d);

// Where the input task puts decoded button events for the render task to
// apply to the UI. Kept separate from the command queue: one is "the rider
// pressed a button", the other is "something asked for a state change", and
// merging them would put UI navigation on the logic task's critical path.
bool post_input_event(const core::nav::InputEvent& e);
bool take_input_event(core::nav::InputEvent& out);

}  // namespace services
