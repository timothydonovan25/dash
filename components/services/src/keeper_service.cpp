// `keeper` — core 1, priority 3, 1 Hz.
//
// The slow, boring, important one: it writes the odometer to NVS, it ticks
// the health tracker, and it retries whatever is faulted. That last job is
// what turns a loose GH1.25 connector into a self-healing problem instead of
// a dead sensor for the rest of the ride (§12).
#include "services/services.hpp"
#include "drivers/mcp23s17.hpp"
#include "drivers/pod.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

namespace services {
namespace {

// Only write the odometer when it has actually moved. NVS wear levelling
// copes with a write per second for the life of the bike; it does not need
// to be asked to.
constexpr double kOdoWriteThresholdMi = 0.01;

void keeper_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::health::Policy policy;
    double last_saved_odo = -1.0;
    double last_saved_hours = -1.0;

    // A shadow copy of the settings as they were last written to flash.
    // Diffing against it once a second is how a changed setting reaches NVS:
    // the config screen edits Settings in place, and nothing on the 50 Hz
    // path should ever touch flash. It also coalesces a rider holding the
    // button and running a value up twenty clicks into a single write.
    core::config::Settings saved;
    bool saved_valid = false;

    for (;;) {
        const uint32_t now = d.clock->now_ms();

        // Notices the devices that went quiet without ever reporting an
        // error, which is the failure mode the pod actually has.
        d.health->tick(now, policy);

        // One retry per faulted device per interval. Each re-init is a few
        // milliseconds on a task nothing waits for.
        if (d.health->claim_retry(core::state::Dev::Imu, now, policy) && d.imu) {
            d.imu->init();
        }
        if (d.health->claim_retry(core::state::Dev::Gnss, now, policy) && d.gnss) {
            d.gnss->init();
        }
        if (d.health->claim_retry(core::state::Dev::McpPod, now, policy)) {
            if (auto* pod = static_cast<drivers::ButtonPod*>(d.pod)) pod->recover_bus();
        }
        if (d.health->claim_retry(core::state::Dev::McpBoard, now, policy) && d.board) {
            d.board->init();
        }
        if (d.health->claim_retry(core::state::Dev::Display, now, policy) && d.panel) {
            d.panel->init();
        }

        // A board expander that browned out comes back with default
        // registers and looks alive. Re-write them whether or not it has
        // complained.
        if (d.board) d.board->refresh_config();

        d.slots->health.publish(d.health->publish());

        // --- life data --------------------------------------------------
        const core::state::Trip t = d.slots->trip.get();
        if (d.storage) {
            const double odo = t.odo_mi;
            const double hours = t.eng_hours;
            if (last_saved_odo < 0.0 ||
                std::fabs(odo - last_saved_odo) >= kOdoWriteThresholdMi) {
                d.storage->write_f64(hal::kNsLife, hal::kKeyOdometerMi, odo);
                last_saved_odo = odo;
            }
            if (last_saved_hours < 0.0 || hours - last_saved_hours >= 0.01) {
                d.storage->write_f64(hal::kNsLife, hal::kKeyEngineHours, hours);
                last_saved_hours = hours;
            }
            // Settings that have changed since the last pass. Comparing
            // through the table rather than memcmp means padding bytes and
            // fields nobody can edit never cause a spurious write.
            if (!saved_valid) {
                saved = *d.settings;
                saved_valid = true;
            } else {
                for (size_t i = 0; i < core::config::item_count(); ++i) {
                    const core::config::ConfigItem& it = core::config::items()[i];
                    if (it.kind == core::config::Kind::Action) continue;
                    const float now_v = core::config::get(*d.settings, it);
                    if (now_v == core::config::get(saved, it)) continue;
                    if (hal::ok(d.storage->write_f32(hal::kNsCfg, it.key, now_v))) {
                        core::config::set(saved, it, now_v);
                    }
                }
            }

            d.storage->commit();
        }

        // --- rails ------------------------------------------------------
        // BRINGUP: there is no VIN sense on this board revision — no divider,
        // and IO6 is the only spare pin with an ADC on it, reserved for
        // exactly that on the next one (dash-v1-netlist-review §3). Until
        // then the firmware genuinely cannot tell bike power from USB power,
        // so these stay at zero and every widget bound to them shows `--`,
        // which is the truth.
        core::state::Elec e;
        d.slots->elec.publish(e);

        // --- system -----------------------------------------------------
        if (d.system) {
            d.system->uptime_s = now / 1000;
            d.system->heap_free_b = static_cast<uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            d.system->heap_min_b = static_cast<uint32_t>(
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            d.system->psram_free_b = static_cast<uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

            // Bus recoveries live in the driver that performs them; the
            // DIAG bus screen is the reason they are counted at all.
            if (auto* pod = static_cast<drivers::ButtonPod*>(d.pod)) {
                d.system->pod_recoveries = pod->recoveries();
                d.system->pod_stuck = pod->stuck();
            }
        }

        // Backlight follows the config setting. There is no light sensor on
        // this board, so auto-dim is not an option yet (§18).
        if (d.panel) d.panel->set_backlight(d.settings->brightness_pct);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

void start_keeper(Deps& d) {
    xTaskCreatePinnedToCore(keeper_task, "keeper", 4096, &d, 3, nullptr, 1);
}

}  // namespace services
