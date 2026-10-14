// Wiring for the ESP32-P4 demo — the same job main/wiring.cpp does for the
// real dash (build every long-lived object once, hand out references, no
// globals.h), trimmed to what a screen-only demo needs. See
// docs/bringup-p4-demo.md for what's unverified about the hardware side of
// this, and the plan behind this folder for what's deliberately not here.
#include "services/services.hpp"
#include "services/logic_tick.hpp"
#include "demo_render.hpp"
#include "demo_bike_task.hpp"
#include "ctrl_probe.hpp"
#include "serial_buttons.hpp"
#include "services/settings_store.hpp"
#include "drivers/nvs_storage.hpp"
#include "p4_panel.hpp"
#include "touch_buttons.hpp"
#include "pod_buttons.hpp"
#include "drivers/clock.hpp"
#include "hal/i_engine.hpp"
#include "core/input/input.hpp"
#include "core/delta/delta.hpp"
#include "p4_pins.hpp"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The same two guards as the real main.cpp: a stale sdkconfig that lost
// either would build clean and test the wrong thing.
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE must be on: delete sdkconfig and rebuild"
#endif
#if !CONFIG_BT_NIMBLE_PINNED_TO_CORE_1
#error "NimBLE must be pinned to core 1: delete sdkconfig and rebuild"
#endif

namespace {

constexpr const char* kTag = "demo_p4";
constexpr uint32_t kTouchPollHz = 100;   // same rate the real pod is polled at

core::state::Slots     g_slots;
core::config::Settings g_settings;
core::health::Tracker  g_health;
ui::SystemInfo         g_system;
services::LapHistory   g_lap_history;
// PSRAM, not internal SRAM — the same reason main/wiring.cpp puts the real
// dash's copy there: 1536 points x 2 (reference + recording) is real size,
// and there's nothing time-critical about the delta screen that would care
// about the slower access.
EXT_RAM_BSS_ATTR core::delta::Tracker g_delta;
EXT_RAM_BSS_ATTR core::state::TrackBuffer<core::state::TrackPoint, 4096> g_trip_trace;
EXT_RAM_BSS_ATTR core::state::TrackBuffer<core::state::TrackPoint, 4096> g_lap_trace;

drivers::EspClock g_clock;
demo::P4Panel     g_panel;
demo::TouchButtons g_touch;
demo::PodButtons   g_pod;     // the real pod on CN3, if plugged in
hal::NullEngine   g_engine;
drivers::NvsStorage g_storage;   // the real driver: settings and BLE bonds   // no ECU here, same as the real board (§10.1)

services::Deps         g_deps;
services::CommandQueue g_commands;

// DIAG -> Sensors' "MCP pod" row (and DIAG -> Bus's "I2C0 pod"). The pod is
// optional here, so a demo that never had one shows it absent, not faulted;
// one that has been seen and then pulled degrades and faults as on the
// bike, and goes back to Ok when it answers again.
void report_pod(hal::Status s, uint32_t now) {
    constexpr auto kDev = core::state::Dev::McpPod;
    if (s == hal::Status::Busy) return;   // no transaction this poll
    if (hal::ok(s)) {
        if (g_health.is_absent(kDev)) g_health.begin_init(kDev, now);
        g_health.report_ok(kDev, now);
    } else if (!g_health.is_absent(kDev)) {
        g_health.report_error(kDev, static_cast<int32_t>(s), now);
    }
}

void touch_input_task(void*) {
    core::input::Decoder decoder;
    core::input::Config  cfg;

    for (;;) {
        // Touch zones and the pod OR into one mask, so one Decoder sees
        // a button as held whichever of them is holding it. A read that
        // fails counts as nothing pressed: the other source keeps working,
        // and a pod pulled out mid-press releases rather than sticks.
        const uint32_t now = g_clock.now_ms();
        uint8_t touch = 0, pod = 0;
        g_touch.read_mask(touch);
        report_pod(g_pod.read_mask(pod, now), now);
        core::nav::InputEvent ev[core::input::kButtonCount];
        const size_t n = decoder.update(touch | pod, now, cfg, ev,
                                        core::input::kButtonCount);
        for (size_t i = 0; i < n; ++i) services::post_input_event(ev[i]);
        vTaskDelay(pdMS_TO_TICKS(1000 / kTouchPollHz));
    }
}

}  // namespace

extern "C" void app_main(void) {
    // Backlight on first, before anything that can hang (panel init still
    // can, on this board — see docs/bringup-p4-demo.md). A lit backlight
    // that never gets a picture at least confirms the FPC and backlight
    // circuit are alive independent of whatever the DSI link is doing.
    const auto bl_gpio = static_cast<gpio_num_t>(p4pins::kPanelBacklight);
    gpio_reset_pin(bl_gpio);
    gpio_set_direction(bl_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(bl_gpio, 1);
    ESP_LOGI(kTag, "backlight: forced on (GPIO%d) before panel init", p4pins::kPanelBacklight);

    const hal::Status panel_status = g_panel.init();
    ESP_LOGI(kTag, "panel: %s", hal::status_name(panel_status));
    g_health.begin_init(core::state::Dev::Display, g_clock.now_ms());
    if (hal::ok(panel_status)) {
        g_health.report_ok(core::state::Dev::Display, g_clock.now_ms());
    } else {
        g_health.report_error(core::state::Dev::Display,
                              static_cast<int32_t>(panel_status), g_clock.now_ms());
    }

    // NVS, then the saved settings, before anything reads them -- the same
    // order and the same code as the real wiring.
    const hal::Status nvs_status = g_storage.init();
    const size_t loaded = hal::ok(nvs_status)
                              ? services::load_settings(g_storage, g_settings) : 0;
    ESP_LOGI(kTag, "nvs: %s, %u saved settings", hal::status_name(nvs_status),
             static_cast<unsigned>(loaded));

    const hal::Status touch_status = g_touch.init();
    ESP_LOGI(kTag, "touch: %s", hal::status_name(touch_status));
    report_pod(g_pod.init(g_touch.bus()), g_clock.now_ms());   // logs its own status

    // No real sensors, no card, no radio — every pointer left null is a
    // device every service already treats as "not fitted" rather than
    // faulted (see the null checks in render_service.cpp/input_service.cpp
    // that this demo's own demo_render.cpp and main.cpp mirror).
    g_deps.clock    = &g_clock;
    g_deps.panel    = &g_panel;
    g_deps.engine   = &g_engine;
    g_deps.slots    = &g_slots;
    g_deps.settings = &g_settings;
    g_deps.health   = &g_health;
    g_deps.storage  = hal::ok(nvs_status) ? &g_storage : nullptr;
    g_deps.system   = &g_system;
    g_deps.lap_history = &g_lap_history;
    g_deps.delta       = &g_delta;
    g_deps.trip_trace  = &g_trip_trace;
    g_deps.lap_trace   = &g_lap_trace;
    g_deps.commands    = &g_commands;

    // demo_render keeps the table from here on; this demo has no keeper.
    g_health.tick(g_clock.now_ms(), core::health::Policy{});
    g_slots.health.publish(g_health.publish());
    g_commands.begin();

    demo::start_ctrl_probe(g_deps);
    demo::start_demo_render(g_deps, g_commands);
    demo::start_demo_bike(g_deps, g_commands);
    xTaskCreatePinnedToCore(touch_input_task, "touch", 3072, nullptr, 9,
                            nullptr, 1);
    demo::start_serial_buttons();

#ifdef DASH_CRASH_TEST
    // A dev image that dies at boot, for testing the fall back to stable:
    // build with -DDASH_CRASH_TEST=1, flash_dev.py it, Boot dev once.
    ESP_LOGE(kTag, "DASH_CRASH_TEST: aborting on purpose");
    vTaskDelay(pdMS_TO_TICKS(1500));
    abort();
#endif

    // The production `ota` and `ble` tasks, by source: stable/dev slots,
    // DIAG -> Update, bonding, and the C6 firmware updater all run here
    // exactly as on the real board. BLE last, as there.
    services::start_ota(g_deps);
    services::start_ble(g_deps);
    services::start_wifi(g_deps);
}
