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
// Core 0 is the real-time island, 3 ms from a tach edge to a shift lamp, and
// the NimBLE host runs at priority 21 — above the 500 Hz `ctrl` task. A
// radio that can preempt that does not belong on it (§11.4).
//
// This is a build failure rather than a line in a document because that is
// what it took to catch: `sdkconfig.defaults` only seeds `sdkconfig` when it
// is first created, so adding the line to the defaults did nothing to a tree
// that already had a config, and it sat at core 0 unnoticed. The same trap
// ate CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY earlier.
//
// There is no controller to pin: it runs on the C6. ESP-Hosted's transport
// tasks have no Kconfig for their core, so `ble` lowers their priority at
// run time instead (ble_service.cpp).
static_assert(CONFIG_BT_NIMBLE_PINNED_TO_CORE == 1,
              "NimBLE host must be pinned to core 1: run idf.py menuconfig, "
              "or delete sdkconfig and let sdkconfig.defaults rebuild it");
#if !CONFIG_BT_CONTROLLER_DISABLED
#error "the BLE controller is the C6's, over esp_hosted; the P4 has none"
#endif
#endif

// Stable and dev firmware (§11.7) rest on the bootloader's rollback: a dev
// image never confirms itself, so any reset returns to stable. Without it a
// dev build that hangs is a USB reflash. Same sdkconfig trap as above, and
// worse, because the bootloader cannot be fixed over the air afterwards.
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE must be on: delete sdkconfig and rebuild"
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
    //
    // ctrl goes first, as soon as the lamps' hardware is up: the shift
    // lights should not wait on the panel's 120 ms sleep-out.
    services::start_ctrl(deps);
    wiring::finish();
    services::start_imu(deps);
    services::start_gnss(deps);
    services::start_logic(deps, commands);
    services::start_input(deps, commands);

    services::start_render(deps, commands);
    services::start_logger(deps);
    services::start_storage(deps);
    services::start_keeper(deps);
    services::start_power(deps);
    services::start_ota(deps);
    // Last, so a radio that is slow to come up, or never does, delays
    // nothing above it (§11.8).
    services::start_ble(deps);
    services::start_wifi(deps);

    ESP_LOGI(kTag, "running");
    // app_main returns and its task is deleted; the eleven above carry on.
}
