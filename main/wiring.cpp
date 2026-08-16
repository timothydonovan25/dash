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

#include "bsp/buses.hpp"
#include "drivers/drivers.hpp"
#include "bsp/pins.hpp"
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

core::state::TrackBuffer<core::state::TrackPoint, 4096> g_trip_trace;
core::state::TrackBuffer<core::state::TrackPoint, 4096> g_lap_trace;
core::log::Ring<core::log::Record, 512>                 g_blackbox;
core::delta::Tracker                                    g_delta;

drivers::EspClock    g_clock;
drivers::Mcp23s17    g_board;
drivers::ButtonPod   g_pod;
drivers::Ili9488     g_panel{&g_board};
drivers::Icm42688p   g_imu;
drivers::Pa1616d     g_gnss;
drivers::EngineSense g_sense{&g_board};
drivers::NvsStorage  g_storage;
drivers::SdCard      g_card;
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

void load_settings() {
    // Every value that has ever been saved comes back through the same table
    // that the config screen renders and that validates the ranges. A key
    // that is not in NVS keeps its default, which is what a fresh board and
    // a newly added setting both look like.
    for (size_t i = 0; i < core::config::item_count(); ++i) {
        const core::config::ConfigItem& it = core::config::items()[i];
        if (it.kind == core::config::Kind::Action) continue;
        float v = 0.0f;
        if (hal::ok(g_storage.read_f32(hal::kNsCfg, it.key, v))) {
            core::config::set(g_settings, it, v);      // clamps on the way in
        }
    }
}

}  // namespace

services::Deps& build() {
    // 1. Buses and safe pin levels, before any driver exists.
    const bsp::BusStatus buses = bsp::board_init();
    ESP_LOGI(kTag, "buses: spi2=%d spi3=%d i2c0=%d uart1=%d",
             buses.spi2, buses.spi3, buses.i2c0, buses.uart1);

    // 2. Storage first, because the settings decide how everything else is
    //    set up.
    bring_up(core::state::Dev::Sd, g_storage.init());
    load_settings();

    // 3. The board expander, then the panel. TFT_RST is on GPB6, so nothing
    //    about the display can happen before the expander answers (§6).
    bring_up(core::state::Dev::McpBoard, g_board.init());
    bring_up(core::state::Dev::Display, g_panel.init());
    g_panel.set_backlight(20);
    g_board.write_shift_lamps(0);

    // 4. The sensors. Any of these may be absent; none of them stop the boot.
    bring_up(core::state::Dev::Imu, g_imu.init());
    bring_up(core::state::Dev::Gnss, g_gnss.init());
    bring_up(core::state::Dev::McpPod, g_pod.init());
    g_sense.init();
    g_card.init();

    // Not fitted, and not faults: the health table distinguishes the two so
    // the keeper does not spend the ride retrying things that do not exist.
    g_health.set_absent(core::state::Dev::Ecu);       // no CAN transceiver
    g_health.set_absent(core::state::Dev::BleHost);   // not in this build
    g_health.set_absent(core::state::Dev::Phone);
    g_health.set_absent(core::state::Dev::TpmsF);
    g_health.set_absent(core::state::Dev::TpmsR);

    // 5. Hand out the references. This is the only place any of these
    //    objects meet each other.
    g_deps.clock      = &g_clock;
    g_deps.imu        = &g_imu;
    g_deps.gnss       = &g_gnss;
    g_deps.board      = &g_board;
    g_deps.pod        = &g_pod;
    g_deps.panel      = &g_panel;
    g_deps.sense      = &g_sense;
    g_deps.engine     = &g_engine;
    g_deps.storage    = &g_storage;
    g_deps.card       = &g_card;
    g_deps.slots      = &g_slots;
    g_deps.settings   = &g_settings;
    g_deps.health     = &g_health;
    g_deps.system     = &g_system;
    g_deps.trip_trace = &g_trip_trace;
    g_deps.lap_trace  = &g_lap_trace;
    g_deps.blackbox   = &g_blackbox;
    g_deps.delta      = &g_delta;

    g_slots.health.publish(g_health.publish());
    g_commands.begin();
    return g_deps;
}

services::CommandQueue& commands() { return g_commands; }

}  // namespace wiring
