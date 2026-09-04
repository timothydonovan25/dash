// Wiring for the ESP32-P4 demo — the same job main/wiring.cpp does for the
// real dash (build every long-lived object once, hand out references, no
// globals.h), trimmed to what a screen-only demo needs. See
// docs/bringup-p4-demo.md for what's unverified about the hardware side of
// this, and the plan behind this folder for what's deliberately not here.
#include "services/services.hpp"
#include "services/logic_tick.hpp"
#include "demo_render.hpp"
#include "demo_bike_task.hpp"
#include "ble_media.hpp"
#include "p4_panel.hpp"
#include "touch_buttons.hpp"
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
hal::NullEngine   g_engine;   // no ECU here, same as the real board (§10.1)

services::Deps         g_deps;
services::CommandQueue g_commands;

void touch_input_task(void*) {
    core::input::Decoder decoder;
    core::input::Config  cfg;

    for (;;) {
        uint8_t mask = 0;
        if (hal::ok(g_touch.read_mask(mask))) {
            core::nav::InputEvent ev[core::input::kButtonCount];
            const size_t n = decoder.update(mask, g_clock.now_ms(), cfg, ev,
                                            core::input::kButtonCount);
            for (size_t i = 0; i < n; ++i) services::post_input_event(ev[i]);
        }
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

    const hal::Status touch_status = g_touch.init();
    ESP_LOGI(kTag, "touch: %s", hal::status_name(touch_status));

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
    g_deps.system   = &g_system;
    g_deps.lap_history = &g_lap_history;
    g_deps.delta       = &g_delta;
    g_deps.trip_trace  = &g_trip_trace;
    g_deps.lap_trace   = &g_lap_trace;

    g_slots.health.publish(g_health.publish());
    g_commands.begin();

    demo::start_demo_render(g_deps, g_commands);
    demo::start_demo_bike(g_deps, g_commands);
    xTaskCreatePinnedToCore(touch_input_task, "touch", 3072, nullptr, 9,
                            nullptr, 1);

    // Last: the render/bike tasks are already running and useful on their
    // own, and this is the one piece of the board that talks to another
    // chip (the C6, over SDIO) rather than just local peripherals — if
    // bring-up here goes wrong, the dashboard itself should already be up.
    demo::ble_media_start();
}
