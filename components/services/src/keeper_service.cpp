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

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include <cstdio>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

namespace services {
namespace {

// Only write the odometer when it has actually moved. NVS wear levelling
// copes with a write per second for the life of the bike; it does not need
// to be asked to.
constexpr double kOdoWriteThresholdMi = 0.01;

constexpr uint32_t kHealthPeriodMs = 50;
constexpr uint32_t kSlowEvery = 1000 / kHealthPeriodMs;   // passes per second

// Is there a core dump in flash, and which task made it? Once, at boot: a
// dump only changes with a crash, and a crash is a reboot. The summary is
// over 1 kB, so it is borrowed from PSRAM for the read and handed back.
void read_crash_dump(ui::SystemInfo& sys) {
    if (esp_core_dump_image_check() != ESP_OK) return;     // none, or damaged
    std::strncpy(sys.crash_task, "?", sizeof sys.crash_task);
    auto* sum = static_cast<esp_core_dump_summary_t*>(
        heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_SPIRAM));
    if (sum) {
        if (esp_core_dump_get_summary(sum) == ESP_OK && sum->exc_task[0]) {
            std::strncpy(sys.crash_task, sum->exc_task, sizeof sys.crash_task - 1);
            sys.crash_task[sizeof sys.crash_task - 1] = '\0';
        }
        heap_caps_free(sum);
    }
    switch (esp_reset_reason()) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            sys.crash_new = true;
            break;
        default:
            break;
    }
}

void keeper_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);
    if (d.system) read_crash_dump(*d.system);

    core::health::Policy policy;
    double last_saved_odo = -1.0;
    double last_saved_hours = -1.0;

    SettingsSaver settings_saver;
    drivers::ChipTemp chip_temp;
    const bool chip_temp_up = hal::ok(chip_temp.init());
    core::sun::AutoDim auto_dim;
    uint32_t pass = 0;
    uint32_t saved_wp_gen = 0;
    uint32_t gnss_log_ms = 0;
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

        // shortcut: bring-up only. The attitude at 20 Hz for tools/imu_view.py,
        // only while a USB host is there to read it (is_connected() costs a
        // little on every tick); remove once DIAG -> IMU is on a panel.
        if (d.system && usb_serial_jtag_is_connected()) {
            const core::state::Attitude a = d.slots->attitude.get();
            printf("IMU %.2f %.2f %.2f %.3f %.3f %.3f %.2f %.1f %u\n",
                   static_cast<double>(a.lean_deg), static_cast<double>(a.pitch_deg),
                   static_cast<double>(a.heading_deg), static_cast<double>(a.g_lon),
                   static_cast<double>(a.g_lat), static_cast<double>(a.g_vert),
                   static_cast<double>(a.yaw_rate_dps), static_cast<double>(a.imu_temp_c),
                   static_cast<unsigned>(d.system->imu_timeouts));
        }

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

            // The GPS on the console every 10 s: with no panel, DIAG -> GPS
            // is not there to read. From here, core 1, not from `gnss`.
            if (now - gnss_log_ms >= 10000) {
                gnss_log_ms = now;
                const core::state::Gnss g = d.slots->gnss.get();
                ESP_LOGI("gnss", "sentences %u, bad checksums %u, %s, sats %u, hdop %.1f",
                         static_cast<unsigned>(d.system->gnss_sentences),
                         static_cast<unsigned>(d.system->gnss_bad_csum),
                         g.fix ? "FIX" : "no fix", static_cast<unsigned>(g.sats),
                         static_cast<double>(g.hdop));
                const core::state::Battery bt = d.slots->battery.get();
                ESP_LOGI("power", "battery %.2f V (VIN + trim), rest %.2f, charge %.2f, crank min %.2f, verdict %u",
                         static_cast<double>(d.slots->elec.get().vbat), static_cast<double>(bt.rest_v),
                         static_cast<double>(bt.charge_v), static_cast<double>(bt.crank_min_v),
                         static_cast<unsigned>(bt.verdict));
                // For a soak on the bench: a leak is a falling `free` here.
                ESP_LOGI("heap", "internal free %u, lowest %u, largest %u; psram free %u",
                         static_cast<unsigned>(d.system->heap_free_b),
                         static_cast<unsigned>(d.system->heap_min_b),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(d.system->psram_free_b));
                ESP_LOGI("imu", "most samples waiting at one wake: %u (the FIFO holds ~128); wakes over 64: %u, over 100: %u",
                         static_cast<unsigned>(d.system->imu_catchup_max),
                         static_cast<unsigned>(d.system->imu_catchup_over64),
                         static_cast<unsigned>(d.system->imu_catchup_over100));
                ESP_LOGI("timing", "ctrl late ticks %u, render %u.%u Hz",
                         static_cast<unsigned>(d.system->ctrl_late),
                         static_cast<unsigned>(d.system->render_hz_x10 / 10),
                         static_cast<unsigned>(d.system->render_hz_x10 % 10));
            }

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
