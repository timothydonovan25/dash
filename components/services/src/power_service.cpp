// `power` — core 1, priority 22, 1 kHz.
//
// Watches VIN for the bike being switched off, and uses the few
// milliseconds of charge left in the bulk capacitors to stop doing damage:
//
//   1. blank the backlight and the lamps, the biggest loads on the rail,
//      which roughly doubles the time left (about 9 ms to 17 ms);
//   2. tell `logger` not to start another SD write;
//   3. write the odometer and engine hours, so the totals are exact rather
//      than up to 0.01 of a mile behind. At once when parked; moving, only
//      once the rail has stayed down a few ms, since that write freezes
//      core 0 and a glitch that comes back must not cost a shift lamp.
//
// It does not flush the log file. 50 ms is what that needs and the board
// has less than half of it (docs/p4-board-pin-map.md, "Hold-up on VIN");
// the blackbox ring and the open file are lost, and the FAT volume may be
// left dirty. That is a hardware limit, not a firmware one.
//
// All of this is UNTESTED ON THE BOARD, and the margin is thin. The first
// bench job is docs/bringup.md "Power-fail": cut the bike supply with USB
// still connected and read the elapsed times this task logs.
#include "services/services.hpp"
#include "core/battery/battery.hpp"
#include "core/power/power.hpp"
#include "bsp/pins.hpp"
#include "drivers/pca9955b.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr const char* kTag = "power";

// 1 kHz needs a 1 ms tick. The sdkconfig sets it; this keeps a change to it
// from silently turning a 1 ms watcher into a 10 ms one.
static_assert(configTICK_RATE_HZ >= 1000, "power needs a 1 ms tick");

// Moving, the totals are saved only once VIN has been below the trip line
// for this many 1 ms samples, the two that tripped it included: 3 ms later
// than parked, out of ~17 ms of hold-up once the lamps and backlight are off.
constexpr uint16_t kConfirmMovingSamples = 5;

void save_totals(Deps& d) {
    if (!d.storage) return;
    const core::state::Trip t = d.slots->trip.get();
    d.storage->write_f64(hal::kNsLife, hal::kKeyOdometerMi, t.odo_mi);
    d.storage->write_f64(hal::kNsLife, hal::kKeyEngineHours, t.eng_hours);
    d.storage->commit();
}

void power_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::power::Divider div;
    div.top_kohm = bsp::kVinTopKohm;
    div.bottom_kohm = bsp::kVinBottomKohm;

    core::power::FailDetector detector;
    TickType_t last = xTaskGetTickCount();
    bool save_pending = false;
    bool save_now = false;
    int64_t trip_us = 0, dark_us = 0;

    // Battery health (core::battery) at 100 Hz on the mean and the lowest
    // VIN of each 10 ms: a starter's dip is a few hundred ms deep and narrow,
    // and this is the one task that samples VIN fast enough to see it; the
    // rest and charging levels are read from the mean, which the ripple does
    // not pull down. Both in battery volts, the trim added.
    core::battery::Monitor battery;
    const core::battery::Config battery_cfg;
    core::state::Battery published;
    float low_v = 0.0f;
    float sum_v = 0.0f;
    uint32_t samples = 0;
    uint32_t last_pub_ms = 0;

    for (;;) {
        vTaskDelayUntil(&last, 1);
        if (!d.rails) continue;

        uint32_t pin_mv = 0;
        if (!hal::ok(d.rails->read_vin_pin_mv_fast(pin_mv))) continue;
        const float vin = core::power::vin_from_pin_mv(pin_mv, div);

        if (samples == 0 || vin < low_v) low_v = vin;
        sum_v = samples == 0 ? vin : sum_v + vin;
        if (++samples >= 10) {
            const uint32_t now = d.clock->now_ms();
            const float trim = d.settings ? d.settings->battery_trim_v : 0.0f;
            battery.update(core::power::battery_v(sum_v / static_cast<float>(samples), trim),
                           core::power::battery_v(low_v, trim),
                           d.slots->engine.get().rpm, now, battery_cfg);
            samples = 0;
            // On change, and once a second regardless so DIAG's averages
            // move: this is the slot's only writer.
            const core::battery::Report& r = battery.report();
            core::state::Battery b;
            b.rest_v = r.rest_v;
            b.crank_min_v = r.crank_min_v;
            b.charge_v = r.charge_v;
            b.verdict = static_cast<uint8_t>(r.verdict);
            if (b.verdict != published.verdict || b.crank_min_v != published.crank_min_v ||
                now - last_pub_ms >= 1000) {
                d.slots->battery.publish(b);
                published = b;
                last_pub_ms = now;
            }
        }

        switch (detector.feed(vin)) {
            case core::power::FailDetector::Event::Tripped: {
                trip_us = esp_timer_get_time();
                // Order is by how much each step buys. Dark first: it is
                // two short I2C writes and it doubles the time left. Direct,
                // not through `ctrl`, which would take up to a tick; and
                // without touching ctrl's lamp state (Pca9955b::blank_lamps).
                d.power_fail.store(true, std::memory_order_relaxed);
                if (d.panel) d.panel->set_backlight(0);
                if (auto* leds = static_cast<drivers::Pca9955b*>(d.lamps)) leds->blank_lamps();
                dark_us = esp_timer_get_time();
                // The save is a flash write, which freezes core 0. Parked,
                // that costs nothing and it goes at once. Moving, a trip can
                // still be a glitch that comes back, and freezing `ctrl`
                // mid-pull for one is the thing every other flash writer is
                // gated against; so wait until the rail has stayed down a
                // few more milliseconds, which a real cut does.
                save_pending = true;
                save_now = still_for_flash(d);
                break;
            }
            case core::power::FailDetector::Event::Recovered:
                // A dip that came back, or USB holding the board up. `ctrl`
                // resends the lamps and the backlight within a few ms.
                d.power_fail.store(false, std::memory_order_relaxed);
                if (save_pending) ESP_LOGW(kTag, "supply back before the save: skipped");
                save_pending = false;
                ESP_LOGW(kTag, "VIN %.1f V: supply back, resuming", static_cast<double>(vin));
                break;
            case core::power::FailDetector::Event::None:
                break;
        }

        if (save_pending && (save_now || detector.low_run() >= kConfirmMovingSamples)) {
            save_pending = false;
            save_totals(d);
            const int64_t saved_us = esp_timer_get_time();
            ESP_LOGW(kTag, "VIN %.1f V: supply cut. dark in %d us, totals saved in %d us%s",
                     static_cast<double>(vin), static_cast<int>(dark_us - trip_us),
                     static_cast<int>(saved_us - trip_us),
                     save_now ? "" : " (moving, after confirming)");
        }
    }
}

}  // namespace

void start_power(Deps& d) {
    // 4 kB: the save path is an NVS write and commit plus a formatted log
    // line, ~2-2.5 kB, and this is the one task whose overflow would land
    // in the middle of a power failure.
    xTaskCreatePinnedToCore(power_task, "power", 4096, &d, 22, nullptr, 1);
}

}  // namespace services
