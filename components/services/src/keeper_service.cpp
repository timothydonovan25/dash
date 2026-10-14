// `keeper` — core 1, priority 3. Health at 20 Hz, everything else at 1 Hz.
//
// The slow, boring, important one: it writes the odometer to NVS, it ticks
// the health tracker, and it retries whatever is faulted. That last job is
// what turns a loose GH1.25 connector into a self-healing problem instead of
// a dead sensor for the rest of the ride (§12).
//
// It is the one task that turns health reports into states and publishes
// them, which is what makes the health table single-writer (rule 2). It
// does that at 20 Hz so a fault reaches `ctrl` and the screen in 50 ms
// rather than a second; the table is only republished when it changes.
#include "services/services.hpp"
#include "services/settings_store.hpp"
#include "drivers/pca9955b.hpp"
#include "drivers/pod.hpp"
#include "core/power/power.hpp"
#include "core/sun/sun.hpp"
#include "bsp/pins.hpp"

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

// The same threshold OTA uses for "not moving" (core::ota::may_write).
constexpr float kStoppedMph = 0.5f;

constexpr uint32_t kHealthPeriodMs = 50;
constexpr uint32_t kSlowEvery = 1000 / kHealthPeriodMs;   // passes per second

void keeper_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::health::Policy policy;
    double last_saved_odo = -1.0;
    double last_saved_hours = -1.0;

    SettingsSaver settings_saver;
    core::sun::AutoDim auto_dim;
    uint32_t pass = 0;
    TickType_t next = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(kHealthPeriodMs));
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
        if (d.health->claim_retry(core::state::Dev::LedDriver, now, policy) && d.lamps) {
            d.lamps->init();
        }
        if (d.health->claim_retry(core::state::Dev::Display, now, policy) && d.panel) {
            d.panel->init();
        }

        d.health->publish_if_changed(d.slots->health, now);

        if (++pass < kSlowEvery) continue;
        pass = 0;

        // An LED driver that browned out comes back with every output at
        // zero current and looks alive. Re-write its configuration whether
        // or not it has complained; `ctrl` re-sends the lamp duty and the
        // backlight duty is re-sent below.
        if (auto* leds = static_cast<drivers::Pca9955b*>(d.lamps)) leds->refresh_config();

        // --- life data --------------------------------------------------
        // Only while stopped. A flash write suspends core 0 for its whole
        // length -- up to ~20 ms when NVS erases a page, every couple of
        // minutes at speed -- and `ctrl` cannot run through it: the shift
        // lights would stall mid-pull. P4 has no flash auto-suspend (IDF
        // v6.0). Stopped with the engine idling is fine: the lamps are dark
        // there, and a setting changed in the garage must not wait for the
        // engine to go off, since `power` saves only the totals at key-off.
        // Miles covered are saved here the first second the bike stops.
        const core::state::Trip t = d.slots->trip.get();
        const bool stopped = d.slots->motion.get().best_mph < kStoppedMph;
        if (d.storage && stopped) {
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
            // Settings that have changed since the last pass.
            settings_saver.save_changed(*d.storage, *d.settings);

            d.storage->commit();
        }

        // --- rails ------------------------------------------------------
        // VIN through the divider. A failed read publishes zero, which every
        // widget shows as `--` and core::power reads as "unknown supply", so
        // `ctrl` falls back to the engine-running test rather than trusting
        // a number that is not there.
        core::state::Elec e;
        uint32_t pin_mv = 0;
        if (d.rails && hal::ok(d.rails->read_vin_pin_mv(pin_mv))) {
            core::power::Divider div;
            div.top_kohm = bsp::kVinTopKohm;
            div.bottom_kohm = bsp::kVinBottomKohm;
            e.vbat = core::power::vin_from_pin_mv(pin_mv, div);
        }
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

        // Backlight follows the config setting, turned down after dark when
        // "Dim at night" is on. The sun comes from the GPS clock and
        // position (core/sun), since the board has no light sensor. Sent
        // every second rather than on change, which is also what restores it
        // after an LED-driver brown-out.
        if (d.panel) {
            const core::state::Gnss g = d.slots->gnss.get();
            const uint8_t dim = auto_dim.update(g.fix && g.day_of_year != 0, g.day_of_year,
                                                g.utc_ms, g.lat, g.lon,
                                                d.settings->dim_at_night);
            // Dark while the supply is failing, so a slow `keeper` pass
            // cannot undo what `power` just did.
            d.panel->set_backlight(
                d.power_fail.load(std::memory_order_relaxed)
                    ? 0
                    : core::sun::apply_dim(d.settings->brightness_pct, dim));
        }

    }
}

}  // namespace

void start_keeper(Deps& d) {
    xTaskCreatePinnedToCore(keeper_task, "keeper", 4096, &d, 3, nullptr, 1);
}

}  // namespace services
