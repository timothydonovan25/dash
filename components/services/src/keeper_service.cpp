// `keeper` — core 1, priority 3. Health at 20 Hz, everything else at 1 Hz.
//
// The slow, boring, important one: it writes the odometer to NVS, it ticks
// the health tracker, and it decides when whatever is faulted is retried
// (the owning task does the retry; see Deps::retry_requests). That last job
// is what turns a loose GH1.25 connector into a self-healing problem instead
// of a dead sensor for the rest of the ride (§12).
//
// It is the one task that turns health reports into states and publishes
// them, which is what makes the health table single-writer (rule 2). It
// does that at 20 Hz so a fault reaches `ctrl` and the screen in 50 ms
// rather than a second; the table is only republished when it changes.
#include "services/services.hpp"
#include "services/settings_store.hpp"
#include "drivers/chip_temp.hpp"
#include "drivers/pod.hpp"
#include "core/power/power.hpp"
#include "core/sun/sun.hpp"
#include "core/waypoint/waypoint.hpp"
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

constexpr uint32_t kHealthPeriodMs = 50;
constexpr uint32_t kSlowEvery = 1000 / kHealthPeriodMs;   // passes per second

void keeper_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::health::Policy policy;
    double last_saved_odo = -1.0;
    double last_saved_hours = -1.0;

    SettingsSaver settings_saver;
    drivers::ChipTemp chip_temp;
    const bool chip_temp_up = hal::ok(chip_temp.init());
    core::sun::AutoDim auto_dim;
    uint32_t pass = 0;
    uint32_t saved_wp_gen = 0;
    TickType_t next = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(kHealthPeriodMs));
        const uint32_t now = d.clock->now_ms();

        // Notices the devices that went quiet without ever reporting an
        // error, which is the failure mode the pod actually has.
        d.health->tick(now, policy);

        // One retry per faulted device per interval. The keeper decides when
        // and the device's own task does it, between its own transactions
        // (Deps::retry_requests): imu, gnss, input (the pod's bus), ctrl
        // (the LED driver) and render (the panel).
        static constexpr core::state::Dev kOwned[] = {
            core::state::Dev::Imu, core::state::Dev::Gnss, core::state::Dev::McpPod,
            core::state::Dev::LedDriver, core::state::Dev::Display};
        for (const core::state::Dev dev : kOwned) {
            if (d.health->claim_retry(dev, now, policy)) request_retry(d, dev);
        }

        d.health->publish_if_changed(d.slots->health, now);

        if (++pass < kSlowEvery) continue;
        pass = 0;

        // --- life data --------------------------------------------------
        // Only while still (core::flash::still). A flash write suspends
        // core 0 for its whole length -- up to ~20 ms when NVS erases a
        // page, every couple of minutes at speed -- and `ctrl` cannot run
        // through it: the shift lights would stall mid-pull. P4 has no flash
        // auto-suspend (IDF v6.0). Stopped with the engine idling is fine:
        // the lamps are dark there, and a setting changed in the garage must
        // not wait for the engine to go off, since `power` saves only the
        // totals at key-off. Revving is not, and neither is a bike whose
        // speedo reads 0 because its wheel sensor is dead: every speed
        // source has to agree. Miles covered are saved here the first
        // second the bike stops.
        const core::state::Trip t = d.slots->trip.get();
        if (d.storage && still_for_flash(d)) {
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

            // The named waypoints, when `logic` has changed one.
            const uint32_t wp_gen = d.slots->waypoint.generation();
            if (wp_gen != saved_wp_gen) {
                const core::state::Waypoints w = d.slots->waypoint.get();
                for (size_t i = 0; i < core::state::Waypoints::kCount; ++i) {
                    const uint16_t k = static_cast<uint16_t>(hal::kKeyWaypointBase + i * 3);
                    const core::state::Waypoint& p = w.pts[i];
                    d.storage->write_f64(hal::kNsLife, k, p.lat);
                    d.storage->write_f64(hal::kNsLife, static_cast<uint16_t>(k + 1), p.lon);
                    d.storage->write_f64(hal::kNsLife, static_cast<uint16_t>(k + 2),
                                         core::waypoint::encode_when(p));
                }
                saved_wp_gen = wp_gen;
            }

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
            // Battery volts: VIN plus the input diode's drop (the trim), so
            // DIAG, the alerts and the bike/USB test all read the battery.
            e.vbat = core::power::battery_v(core::power::vin_from_pin_mv(pin_mv, div),
                                            d.settings->battery_trim_v);
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
            float c = 0.0f;
            const bool ok = chip_temp_up && hal::ok(chip_temp.read(c));
            if (ok) d.system->chip_temp_c = c;
            d.system->chip_temp_ok = ok;

            watch_stacks(*d.system, now);

            // Bus recoveries live in the driver that performs them; the
            // DIAG bus screen is the reason they are counted at all.
            if (auto* pod = static_cast<drivers::ButtonPod*>(d.pod)) {
                d.system->pod_recoveries = pod->recoveries();
                d.system->pod_stuck = pod->stuck();
            }
        }

        // Backlight follows the config setting, turned down after dark when
        // "Dim at night" is on. The sun comes from the GPS clock and
        // position (core/sun), since the board has no light sensor. Decided
        // here and sent by `ctrl`, the one writer of the LED driver, which
        // also keeps it dark while the supply is failing and re-sends it
        // once a second to undo a brown-out.
        {
            const core::state::Gnss g = d.slots->gnss.get();
            const uint8_t dim = auto_dim.update(g.fix && g.day_of_year != 0, g.day_of_year,
                                                g.utc_ms, g.lat, g.lon,
                                                d.settings->dim_at_night);
            d.backlight_pct.store(core::sun::apply_dim(d.settings->brightness_pct, dim),
                                  std::memory_order_relaxed);
        }

    }
}

}  // namespace

void start_keeper(Deps& d) {
    xTaskCreatePinnedToCore(keeper_task, "keeper", 4096, &d, 3, nullptr, 1);
}

}  // namespace services
