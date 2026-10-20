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
// | ctrl    |  0   |  18  | 500 Hz    | tach, wheel speed, shift lamps,     |
// |         |      |      | + tach    |   every LED-driver write            |
// | imu     |  0   |  15  | 200 Hz    | FIFO drain, attitude, jumps         |
// | gnss    |  0   |  12  | UART      | NMEA parse, position                |
// | logic   |  0   |  10  | 50 Hz     | lap timer, trip, command drain      |
// | input   |  0   |   9  | 100 Hz    | pod poll, debounce                  |
// | render  |  1   |   6  | 60 Hz     | snapshot, widgets, panel            |
// | ble     |  1   |   4  | event     | media GATT server, BLE diag         |
// | keeper  |  1   |   3  | 1 Hz      | NVS, health retry, VIN sense        |
// | storage |  1   |   3  | 2 Hz      | card mount, hotplug, USB hand-off    |
// | logger  |  1   |   2  | drain     | blackbox ring to SD                 |
//
// Core 0 is a real-time island: no radio, no filesystem. Anything added to
// it above priority 18 breaks the 3 ms tach-to-lamp budget.
//
// The radio's own tasks — the NimBLE host and ESP-Hosted's SDIO transport —
// are not in this table because the IDF and esp_hosted create them. NimBLE
// is pinned to core 1 by sdkconfig; `ble` lowers ESP-Hosted's below every
// core-0 task, because it creates them at priority 23 with no affinity.
#pragma once
#include <atomic>
#include "hal/hal.hpp"
#include "core/state/slots.hpp"
#include "core/state/track_buffer.hpp"
#include "core/config/config.hpp"
#include "core/health/health.hpp"
#include "core/log/log.hpp"
#include "core/log/detail.hpp"
#include "core/delta/delta.hpp"
#include "core/flash/flash.hpp"
#include "services/track_store.hpp"
#include "ui/screen.hpp"

namespace services {

// Every lap scored this session. Fixed size: more laps than this on one
// tank is not a thing a CRF250F does.
struct LapHistory {
    static constexpr size_t kMax = 64;
    uint32_t ms[kMax] = {};
    core::state::LapPeak peak[kMax] = {};
    size_t   count = 0;
    uint32_t best_ms = 0;
};

// Everything every task needs, wired once in main and passed by reference.
// This struct is why there is no `globals.h` in this repo.
struct Deps {
    hal::IClock*       clock   = nullptr;
    hal::IImu*         imu     = nullptr;
    hal::IGnss*        gnss    = nullptr;
    hal::IButtonPod*   pod     = nullptr;
    hal::IPanel*       panel   = nullptr;
    hal::IEngineSense* sense   = nullptr;
    hal::IShiftLamps*  lamps   = nullptr;
    hal::IEngine*      engine  = nullptr;      // NullEngine today
    hal::IStorage*     storage = nullptr;
    hal::ICard*        card    = nullptr;
    // A build that runs a game off the card (the demo board's Doom): the
    // Wi-Fi page then lists, uploads and deletes .wad files in games/.
    bool               games   = false;
    hal::IRailSense*   rails   = nullptr;

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

    // The detail log (core/log/detail.hpp), one ring per producer, all
    // drained by `logger` into detail/. Pushed only while CONFIG -> Log ->
    // Detail log is on. Any of them may be null (the sim, a test), and its
    // producer then skips it.
    core::log::ImuRing*  imu_log  = nullptr;   // imu, 200 Hz raw samples
    core::log::TachRing* tach_log = nullptr;   // ctrl, 50 Hz pulses and timing
    core::log::DetRing*  det_log  = nullptr;   // logic, 50 Hz filter state
    core::log::EvtRing*  evt_log  = nullptr;   // logic, events

    // Lap delta against the session best. Two point traces, so it is far too
    // big for a task stack and lives here like the others.
    core::delta::Tracker* delta = nullptr;

    // Lap times for the Track history screen. The logic task scores them and
    // the render task draws them, so they live here rather than inside
    // either — there was no path between the two before, and the screen
    // showed "no laps yet" for the whole session.
    LapHistory* lap_history = nullptr;

    // Saved tracks, between `logic` (core 0) and `storage`, which owns the
    // card (services/track_store.hpp). In PSRAM: two lap traces' worth.
    TrackMailbox* tracks = nullptr;

    // Bytes of a firmware upload, from the Wi-Fi page to `ota`. A FreeRTOS
    // StreamBuffer, made by start_ota; void* so this header stays free of
    // FreeRTOS types.
    void* ota_stream = nullptr;

    // The command queue, for a task that is not handed it at start: the
    // Wi-Fi page posts ConfigSet through it like the config screen does.
    ui::ICommandSink* commands = nullptr;

    // Set by `power` when VIN has just been cut and the board is running on
    // what is left in its capacitors. Everything that writes or draws power
    // stands down while it is set: `logger` stops starting SD writes, `ctrl`
    // keeps the lamps dark, `keeper` keeps the backlight off. Cleared if the
    // supply comes back, or if the board turns out to be on USB.
    std::atomic<bool> power_fail{false};

    // Re-inits `keeper` has decided on, one bit per core::state::Dev. The
    // keeper owns the retry *policy* (core::health::claim_retry); the task
    // that owns the device does the retry, between its own transactions. A
    // re-init from another core used to land in the middle of them: an IMU
    // soft reset under a FIFO read, a UART baud change under the GPS parser,
    // a panel reset mid-frame, an I2C driver deleted under the pod poll.
    std::atomic<uint32_t> retry_requests{0};

    // The backlight duty, in percent, that `keeper` wants (setting, night
    // dim, power fail). `ctrl` writes it: it is the one task that talks to
    // the LED driver, so a core-1 write can never hold I2C1 while the lamps
    // are waiting for it.
    std::atomic<uint8_t> backlight_pct{20};

    // The `logic` task, once it is running; TaskHandle_t. See wake_logic.
    std::atomic<void*> logic_task{nullptr};
};

// Run a `logic` pass now rather than at its next 50 Hz tick: a producer has
// published something `logic` turns into what the rider sees (a new fix).
// Never blocks; a no-op before `logic` has started. Not for ctrl's 500 Hz
// motion publish -- that would make `logic` a 500 Hz task.
void wake_logic(Deps& d);

inline void request_retry(Deps& d, core::state::Dev dev) {
    d.retry_requests.fetch_or(1u << static_cast<unsigned>(dev));
}

// The owning task: true once per request.
inline bool take_retry(Deps& d, core::state::Dev dev) {
    const uint32_t bit = 1u << static_cast<unsigned>(dev);
    return (d.retry_requests.fetch_and(~bit) & bit) != 0;
}
static_assert(core::state::kDevCount <= 32, "retry_requests is one 32-bit mask");

// core::flash::still, read off the slots: may a task write the P4's flash
// now without stalling `ctrl` mid-pull? `keeper` and NimBLE's bond store ask
// this; OTA asks the stricter core::ota::may_write.
inline bool still_for_flash(const Deps& d) {
    const core::state::Motion m = d.slots->motion.get();
    const core::state::Gnss g = d.slots->gnss.get();
    core::flash::Stillness s;
    s.front_mph = m.front_mph;
    s.rear_mph = m.rear_mph;
    s.gps_mph = g.speed_mph;
    s.gps_fix = g.fix;
    s.rpm = d.slots->engine.get().rpm;
    s.lamp_base_rpm = core::config::shift_view(*d.settings).base_rpm;
    return core::flash::still(s);
}

// The one command queue (§4.5). Screens post, `logic` drains, and every
// command that changed anything can be logged in one place.
class CommandQueue final : public ui::ICommandSink {
public:
    bool begin(size_t depth = 16);
    bool post(const core::state::Command& c) override;      // any task
    bool post_from_isr(const core::state::Command& c);
    bool take(core::state::Command& out);                   // logic only
    uint32_t dropped() const { return dropped_; }
    // The task to wake when a command lands, so `logic` acts on it at once
    // rather than at its next scheduled tick. Set by `logic` itself.
    void set_consumer(void* task) { consumer_.store(task, std::memory_order_relaxed); }

private:
    void*    q_ = nullptr;          // QueueHandle_t
    uint32_t dropped_ = 0;
    std::atomic<void*> consumer_{nullptr};   // TaskHandle_t
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
void start_power(Deps& d);
void start_logger(Deps& d);
void start_storage(Deps& d);
// Stable/dev slots and updates (§11.7). Core 1, priority 2.
void start_ota(Deps& d);
void start_ble(Deps& d);
// ESP-Hosted makes its SDIO and RPC tasks at priority 23 with no core
// affinity: above `ctrl`, and free to run on core 0. This lowers whichever
// exist below every core-0 task. `report` logs the count even if nothing
// needed lowering (the bring-up check that the task names still match).
void demote_radio_tasks(bool report);

// The task watchdog (watchdog.cpp). A task that joins must feed it at
// least once every CONFIG_ESP_TASK_WDT_TIMEOUT_S (5 s), or the board
// panics and reboots: ctrl, imu, logic and render, once a loop each.
void watchdog_join();
void watchdog_feed();

// Every task's stack high-water mark into DIAG -> System (the tightest
// task) and the console (stack_watch.cpp). Once a second, from keeper.
void watch_stacks(ui::SystemInfo& sys, uint32_t now_ms);
// The on-demand update hotspot and its page (§11.7). Core 1, priority 3.
// Off until the rider turns it on. Starts nothing on the radio until then,
// but needs esp_hosted up, so it goes after start_ble.
void start_wifi(Deps& d);

// Where the input task puts decoded button events for the render task to
// apply to the UI. Kept separate from the command queue: one is "the rider
// pressed a button", the other is "something asked for a state change", and
// merging them would put UI navigation on the logic task's critical path.
bool post_input_event(const core::nav::InputEvent& e);
bool take_input_event(core::nav::InputEvent& out);

// Blocks until a button event arrives or `timeout_ms` passes. This is what
// the render task waits on instead of a plain periodic delay: a press wakes
// the frame immediately rather than waiting out the rest of the period.
bool wait_input_event(core::nav::InputEvent& out, uint32_t timeout_ms);

}  // namespace services
