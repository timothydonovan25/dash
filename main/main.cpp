// Entry point. What happens, in what order, and which task runs where.
//
// The objects themselves are built in wiring.cpp — this file is the boot
// sequence and nothing else, so that "why did it come up like that" has one
// short file to read.
#include "wiring.hpp"

#include "core/config/config.hpp"
#include "esp_log.h"

namespace {
constexpr const char* kTag = "dash";
}

#if CONFIG_BT_ENABLED
// The BLE controller runs at priority 23, above the 500 Hz `ctrl` task, and
// core 0 is the real-time island with 3 ms from a tach edge to a shift lamp.
// A radio that can preempt that does not belong on it (§11.4).
//
// This is a build failure rather than a line in a document because that is
// what it took to catch: `sdkconfig.defaults` only seeds `sdkconfig` when it
// is first created, so adding these two lines to the defaults did nothing to
// any tree that already had a config — and both were quietly sitting at core
// 0. The same trap ate CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY earlier.
// Nothing starts the radio yet, so neither had any symptom to notice.
static_assert(CONFIG_BT_CTRL_PINNED_TO_CORE == 1,
              "BLE controller must be pinned to core 1: run idf.py menuconfig, "
              "or delete sdkconfig and let sdkconfig.defaults rebuild it");
static_assert(CONFIG_BT_NIMBLE_PINNED_TO_CORE == 1,
              "NimBLE host must be pinned to core 1: run idf.py menuconfig, "
              "or delete sdkconfig and let sdkconfig.defaults rebuild it");
#endif

extern "C" void app_main() {
    ESP_LOGI(kTag, "dash v2 — %u config items",
             static_cast<unsigned>(core::config::item_count()));

    // A malformed config table otherwise shows up weeks later as a setting
    // that will not save. Say so at boot instead.
    const char* why = nullptr;
    if (!core::config::validate_table(&why)) {
        ESP_LOGE(kTag, "config table: %s", why ? why : "invalid");
    }

    services::Deps& deps = wiring::build();
    services::CommandQueue& commands = wiring::commands();

    // Core 0 is the real-time island — ctrl, imu, gnss, logic, input, and
    // nothing else. Core 1 takes everything that can afford to be late.
    // Anything added to core 0 above priority 18 breaks the 3 ms budget from
    // a tach edge to a shift lamp.
    services::start_ctrl(deps);
    services::start_imu(deps);
    services::start_gnss(deps);
    services::start_logic(deps, commands);
    services::start_input(deps, commands);

    services::start_render(deps, commands);
    services::start_logger(deps);
    services::start_storage(deps);
    services::start_keeper(deps);

    ESP_LOGI(kTag, "running");
    // app_main returns and its task is deleted; the nine above carry on.
}
