// `power` — core 1, priority 22, 1 kHz.
//
// Watches VIN for the bike being switched off, and uses the few
// milliseconds of charge left in the bulk capacitors to stop doing damage:
//
//   1. blank the backlight and the lamps, the biggest loads on the rail,
//      which roughly doubles the time left (about 9 ms to 17 ms);
//   2. tell `logger` not to start another SD write;
//   3. write the odometer and engine hours, so the totals are exact rather
//      than up to 0.01 of a mile behind.
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
#include "core/power/power.hpp"
#include "bsp/pins.hpp"

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

    for (;;) {
        vTaskDelayUntil(&last, 1);
        if (!d.rails) continue;

        uint32_t pin_mv = 0;
        if (!hal::ok(d.rails->read_vin_pin_mv_fast(pin_mv))) continue;
        const float vin = core::power::vin_from_pin_mv(pin_mv, div);

        switch (detector.feed(vin)) {
            case core::power::FailDetector::Event::Tripped: {
                const int64_t t0 = esp_timer_get_time();
                // Order is by how much each step buys. Dark first: it is
                // two short I2C writes and it doubles the time left.
                d.power_fail.store(true, std::memory_order_relaxed);
                if (d.panel) d.panel->set_backlight(0);
                if (d.lamps) d.lamps->set_master(0);
                const int64_t t1 = esp_timer_get_time();
                save_totals(d);
                const int64_t t2 = esp_timer_get_time();
                ESP_LOGW(kTag, "VIN %.1f V: supply cut. dark in %d us, totals saved in %d us",
                         static_cast<double>(vin), static_cast<int>(t1 - t0),
                         static_cast<int>(t2 - t0));
                break;
            }
            case core::power::FailDetector::Event::Recovered:
                // A dip that came back, or USB holding the board up. `keeper`
                // resends the backlight within a second and `ctrl` the lamps
                // within a few milliseconds.
                d.power_fail.store(false, std::memory_order_relaxed);
                ESP_LOGW(kTag, "VIN %.1f V: supply back, resuming", static_cast<double>(vin));
                break;
            case core::power::FailDetector::Event::None:
                break;
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
