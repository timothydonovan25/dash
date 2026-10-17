// The one place drivers meet services (§3).
//
// Every long-lived object in the firmware is defined here, and every
// reference between them is set here. Nothing else in the codebase knows
// that an `IImu` happens to be an ICM-42688-P, or that the odometer is
// stored by NVS rather than by something else.
//
// This is rule 1 in its concrete form. v1 had a `globals.h` with sixty
// externs; the replacement is not "no state", it is "all the state in one
// function, handed out by reference".
#include "wiring.hpp"
#include "services/settings_store.hpp"

#include "bsp/buses.hpp"
#include "drivers/drivers.hpp"
#include "bsp/pins.hpp"
#include "esp_attr.h"
#include "esp_log.h"

namespace wiring {
namespace {

constexpr const char* kTag = "wiring";

// Alive for as long as the bike is. Static rather than on app_main's stack,
// which is a few kilobytes and much smaller than these.
core::state::Slots     g_slots;
core::config::Settings g_settings;
core::health::Tracker  g_health;
ui::SystemInfo         g_system;

// The bulk buffers go in PSRAM. Together they are about 190 kB, which is a
// large share of the 512 kB of internal SRAM, and none of them is on a path
// that cares: the traces are walked by a map screen, the blackbox is drained
// by the logger, and the delta reference is searched ten times a second.
// Internal RAM is better spent on the panel's DMA buffer and the stacks.
EXT_RAM_BSS_ATTR core::state::TrackBuffer<core::state::TrackPoint, 4096> g_trip_trace;
EXT_RAM_BSS_ATTR core::state::TrackBuffer<core::state::TrackPoint, 4096> g_lap_trace;
EXT_RAM_BSS_ATTR core::log::Ring<core::log::Record, 512>                 g_blackbox;
EXT_RAM_BSS_ATTR core::delta::Tracker                                    g_delta;
services::LapHistory                                    g_lap_history;

drivers::EspClock    g_clock;
drivers::ButtonPod   g_pod;
drivers::Pca9955b    g_leds;      // shift lamps and backlight
drivers::Icm42688p   g_imu;
drivers::Pa1616d     g_gnss;
drivers::EngineSense g_sense;
drivers::NvsStorage  g_storage;
drivers::SdCard      g_card;
drivers::VinSense    g_vin;
hal::NullEngine      g_engine;   // the ECU is unreachable on this board (§10.1)

services::Deps         g_deps;
services::CommandQueue g_commands;

// Bring one device up and record what happened. Nothing here fails the boot:
// a dash that refuses to start because the GPS is unplugged is worse than a
// dash with no GPS, and every screen already renders `--` for a dead device.
void bring_up(core::state::Dev dev, hal::Status s) {
    const uint32_t now = g_clock.now_ms();
    g_health.begin_init(dev, now);
    if (hal::ok(s)) {
        g_health.report_ok(dev, now);
        ESP_LOGI(kTag, "%s: ok", core::state::dev_name(dev));
    } else {
        g_health.report_error(dev, static_cast<int32_t>(s), now);
        ESP_LOGW(kTag, "%s: %s", core::state::dev_name(dev), hal::status_name(s));
    }
}

}  // namespace

services::Deps& build() {
    // 1. Buses and safe pin levels, before any driver exists.
    const bsp::BusStatus buses = bsp::board_init();
    ESP_LOGI(kTag, "buses: spi2=%d i2c0=%d i2c1=%d uart1=%d lcd=%d",
             buses.spi2, buses.i2c0, buses.i2c1, buses.uart1,
             buses.lcd_bus != nullptr);

    // The panel needs the 8080 bus handle, which only exists once the bus
    // is up. A function-local static is still built exactly once and never
    // freed, like everything else in this file.
    static drivers::Ili9488 panel{buses.lcd_bus, &g_leds};

    // 2. Storage first, because the settings decide how everything else is
    //    set up.
    bring_up(core::state::Dev::Sd, g_storage.init());
    services::load_settings(g_storage, g_settings);

    // 3. What the shift lamps need, and nothing else: the LED driver, the
    //    tach counters and VIN sense. main starts `ctrl` the moment this
    //    returns, so the lamps are live before the panel has woken up. The
    //    driver comes up dark, every duty at zero.
    bring_up(core::state::Dev::LedDriver, g_leds.init());
    // No health slot of its own: a failed init makes every read() say
    // NotPresent, so `ctrl` drops the wheels out of the speedo.
    if (!hal::ok(g_sense.init())) ESP_LOGE(kTag, "engine sense: tach and wheel inputs not hooked up");
    // VIN sense has no health slot: a failed init leaves `rails` reading
    // nothing, which ctrl and the screens already treat as "unknown supply".
    if (!hal::ok(g_vin.init())) ESP_LOGW(kTag, "VIN sense: no ADC");

    // Not here, on purpose: the GPS and the SD card. The GPS spends half a
    // second agreeing a baud rate and the card can take longer than that to
    // mount, so `gnss` and `storage` bring them up on their own tasks while
    // the dash is already running. Neither is needed for the first frame.

    // Not fitted, and not faults: the health table distinguishes the two so
    // the keeper does not spend the ride retrying things that do not exist.
    g_health.set_absent(core::state::Dev::Ecu);       // no CAN transceiver
    g_health.set_absent(core::state::Dev::Phone);     // until one connects
    g_health.set_absent(core::state::Dev::TpmsF);
    g_health.set_absent(core::state::Dev::TpmsR);     // no TPMS scan yet (§11.2)

    // Hand out the references. This is the only place any of these
    //    objects meet each other.
    g_deps.clock      = &g_clock;
    g_deps.imu        = &g_imu;
    g_deps.gnss       = &g_gnss;
    g_deps.pod        = &g_pod;
    g_deps.panel      = &panel;
    g_deps.sense      = &g_sense;
    g_deps.lamps      = &g_leds;
    g_deps.engine     = &g_engine;
    g_deps.storage    = &g_storage;
    g_deps.card       = &g_card;
    g_deps.rails      = &g_vin;
    g_deps.slots      = &g_slots;
    g_deps.settings   = &g_settings;
    g_deps.health     = &g_health;
    g_deps.system     = &g_system;
    g_deps.trip_trace = &g_trip_trace;
    g_deps.lap_trace  = &g_lap_trace;
    g_deps.blackbox   = &g_blackbox;
    g_deps.delta      = &g_delta;
    g_deps.lap_history= &g_lap_history;
    g_deps.commands   = &g_commands;

    // States are derived on a tick, which is keeper's job once it runs;
    // until start_keeper() this boot thread is the only one ticking.
    g_health.tick(g_clock.now_ms(), core::health::Policy{});
    g_slots.health.publish(g_health.publish());
    g_commands.begin();
    return g_deps;
}

void finish() {
    // 4. The panel, then the sensors render and input need. The panel's
    //    reset is a plain GPIO on this board, and its backlight is three of
    //    the LED driver's channels, which build() has already brought up.
    bring_up(core::state::Dev::Display, g_deps.panel->init());
    g_deps.panel->set_backlight(20);

    // Any of these may be absent; none of them stop the boot.
    bring_up(core::state::Dev::Imu, g_imu.init());
    bring_up(core::state::Dev::McpPod, g_pod.init());
    g_health.tick(g_clock.now_ms(), core::health::Policy{});
    g_slots.health.publish(g_health.publish());
}

services::CommandQueue& commands() { return g_commands; }

}  // namespace wiring
