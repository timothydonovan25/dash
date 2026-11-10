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
constexpr uint32_t kStatusPeriodMs = 5000;

const char* dev_state(const core::state::Health& h, core::state::Dev dev) {
    switch (h[dev].state) {
        case core::state::DevState::Absent:   return "absent";
        case core::state::DevState::Init:     return "init";
        case core::state::DevState::Ok:       return "ok";
        case core::state::DevState::Degraded: return "degraded";
        case core::state::DevState::Fault:    return "FAULT";
    }
    return "?";
}

// Every 5 s on the USB console, one line per service, from the slots and
// SystemInfo: what a soak or a bench session watches (docs/console.md).
void print_status(Deps& d, uint32_t now) {
    using core::state::Dev;
    const ui::SystemInfo& s = *d.system;
    const core::state::Health h = d.slots->health.get();
    const core::state::Engine e = d.slots->engine.get();
    const core::state::Motion m = d.slots->motion.get();
    const core::state::Gnss g = d.slots->gnss.get();
    const core::state::Fused f = d.slots->fused.get();
    const core::state::Trip t = d.slots->trip.get();
    const core::state::Battery bt = d.slots->battery.get();
    const core::state::Ota o = d.slots->ota.get();
    const core::state::BleDiag b = d.slots->ble_diag.get();
    const core::state::Heart hr = d.slots->heart.get();
    const core::state::Wifi w = d.slots->wifi.get();
    const auto lost = [](const auto* r) { return r ? static_cast<unsigned>(r->dropped()) : 0u; };
    const auto u = [](auto v) { return static_cast<unsigned>(v); };
    const auto f1 = [](float v) { return static_cast<double>(v); };

    printf("---- status at %u s ----\n", u(now / 1000));
    printf("ctrl    lamps %s, late ticks %u, rpm %.0f, gear %u%s, tps %.0f%%, front %.1f rear %.1f mph\n",
           dev_state(h, Dev::LedDriver), u(s.ctrl_late), f1(e.rpm), u(e.gear),
           e.neutral ? " (N)" : "", f1(e.tps_pct), f1(m.front_mph), f1(m.rear_mph));
    printf("imu     %s, timeouts %u, most waiting %u (FIFO ~128), wakes >64 %u, >100 %u\n",
           dev_state(h, Dev::Imu), u(s.imu_timeouts), u(s.imu_catchup_max),
           u(s.imu_catchup_over64), u(s.imu_catchup_over100));
    printf("gnss    %s, %s, sats %u, hdop %.1f, %.6f %.6f, %.1f mph, sentences %u, bad checksums %u\n",
           dev_state(h, Dev::Gnss), g.fix ? "FIX" : "no fix", u(g.sats), f1(g.hdop), g.lat, g.lon,
           f1(g.speed_mph), u(s.gnss_sentences), u(s.gnss_bad_csum));
    printf("logic   fused %s%.1f mph, resets %u, odo %.2f mi, trip A %.2f mi, engine %.2f h\n",
           f.speed_valid ? "" : "(invalid) ", f1(f.speed_valid ? f.speed_mph : 0.0f), u(f.resets),
           f1(t.odo_mi), f1(t.trip_a_mi), f1(t.eng_hours));
    printf("input   pod %s, bus recoveries %u, stuck %u\n",
           dev_state(h, Dev::McpPod), u(s.pod_recoveries), u(s.pod_stuck));
    printf("render  panel %s, %u.%u Hz, %u Hz sent\n",
           dev_state(h, Dev::Display), u(s.render_hz_x10 / 10), u(s.render_hz_x10 % 10),
           u(s.render_sent_hz));
    printf("logger  rows %u, %u bytes, %u errors, slowest %u ms, dropped %u/%u/%u/%u/%u (bb/imu/tach/det/evt)\n",
           u(s.log_records), u(s.log_bytes), u(s.log_errors), u(s.log_max_ms), lost(d.blackbox),
           lost(d.imu_log), lost(d.tach_log), lost(d.det_log), lost(d.evt_log));
    printf("storage sd %s%s%s\n", dev_state(h, Dev::Sd),
           s.sd_ejected ? ", ejected" : "", s.sd_usb ? ", USB drive" : "");
    printf("keeper  heap %u free, %u lowest, %u largest; psram %u free; chip %.1f C; stack low %s %u B\n",
           u(s.heap_free_b), u(s.heap_min_b), u(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           u(s.psram_free_b), f1(s.chip_temp_ok ? s.chip_temp_c : 0.0f), s.stack_low_task,
           u(s.stack_low_free_b));
    printf("power   battery %.2f V, rest %.2f, charge %.2f, crank min %.2f, verdict %u%s\n",
           f1(d.slots->elec.get().vbat), f1(bt.rest_v), f1(bt.charge_v), f1(bt.crank_min_v),
           u(bt.verdict), d.power_fail.load(std::memory_order_relaxed) ? ", SUPPLY CUT" : "");
    printf("ota     %s %s from %s, install %u (%u%%), error %u\n",
           core::ota::role_name(o.role), o.running_ver, o.running_slot, u(o.install),
           u(o.install_pct), u(o.error));
    printf("ble     host %s, phone %s, rssi %d, bonds %u, watch link %u, %u bpm\n",
           dev_state(h, Dev::BleHost), dev_state(h, Dev::Phone), b.has_rssi ? b.rssi_dbm : 0,
           u(b.bonds), u(hr.link), u(hr.bpm));
    printf("wifi    %s, %u clients\n", core::wifi::state_name(w.state), u(w.clients));
}

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
    // The screen as last saved, so an unchanged one is never rewritten.
    float saved_place = -1.0f;
    if (d.storage) d.storage->read_f32(hal::kNsLife, hal::kKeyUiPlace, saved_place);

    SettingsSaver settings_saver;
    drivers::ChipTemp chip_temp;
    const bool chip_temp_up = hal::ok(chip_temp.init());
    core::sun::AutoDim auto_dim;
    uint32_t pass = 0;
    uint32_t saved_wp_gen = 0;
    uint32_t status_ms = 0;
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
            // Not if they were never read back (Deps::totals_unread).
            const bool totals = !d.totals_unread.load(std::memory_order_relaxed);
            if (totals && (last_saved_odo < 0.0 ||
                           std::fabs(odo - last_saved_odo) >= kOdoWriteThresholdMi)) {
                d.storage->write_f64(hal::kNsLife, hal::kKeyOdometerMi, odo);
                last_saved_odo = odo;
            }
            if (totals && (last_saved_hours < 0.0 || hours - last_saved_hours >= 0.01)) {
                d.storage->write_f64(hal::kNsLife, hal::kKeyEngineHours, hours);
                last_saved_hours = hours;
            }
            const float place = d.ui_place.load(std::memory_order_relaxed);
            if (place != 65535.0f && place != saved_place) {
                d.storage->write_f32(hal::kNsLife, hal::kKeyUiPlace, place);
                saved_place = place;
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

            // The console's one periodic output: a line per service.
            if (now - status_ms >= kStatusPeriodMs) {
                status_ms = now;
                if (usb_serial_jtag_is_connected()) print_status(d, now);
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
