// The 3 ms tach-to-lamp budget (CLAUDE.md) rests on `ctrl` waking on time.
// That cannot be measured end to end without the real board's tach and
// PCA9955B, but the half that is the chip's -- how late a priority-18 task
// on core 0 wakes while everything else is busy -- can, here.
//
// Same period, core, priority and wait as ctrl_service.cpp. The gap between
// wakes is timed with esp_timer (microseconds, not ticks), and
// every ten seconds the worst and a histogram go to the log. DIAG -> System
// shows the late count the same way it shows ctrl's.
#include "ctrl_probe.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cinttypes>

namespace demo {
namespace {

constexpr const char* kTag = "ctrl_probe";
constexpr TickType_t kPeriod = pdMS_TO_TICKS(2);      // 500 Hz, as ctrl
constexpr int64_t    kPeriodUs = 2000;
constexpr uint32_t   kReportMs = 10000;

void probe_task(void* arg) {
    services::Deps& d = *static_cast<services::Deps*>(arg);

    // Lateness buckets, in microseconds past the deadline.
    constexpr int64_t kEdges[] = {50, 200, 500, 1000, 3000};
    constexpr int kBuckets = sizeof(kEdges) / sizeof(kEdges[0]) + 1;
    uint32_t hist[kBuckets] = {};
    int64_t worst_us = 0;
    int64_t worst_ever_us = 0;
    uint32_t ticks = 0;
    uint32_t late = 0;

    TickType_t next = xTaskGetTickCount();
    int64_t last_wake = esp_timer_get_time();
    int64_t last_report = last_wake;

    for (;;) {
        vTaskDelayUntil(&next, kPeriod);
        const int64_t now = esp_timer_get_time();
        // How much longer than one period since the last wake. A tick that
        // woke late shows here; the next one, on time against the tick grid,
        // shows as short and counts as zero.
        int64_t lateness = (now - last_wake) - kPeriodUs;
        if (lateness < 0) lateness = 0;
        last_wake = now;

        int b = 0;
        while (b < kBuckets - 1 && lateness >= kEdges[b]) ++b;
        ++hist[b];
        ++ticks;
        if (lateness > worst_us) worst_us = lateness;
        if (lateness >= 1000) {
            ++late;
            if (d.system) d.system->ctrl_late = static_cast<uint16_t>(late);
        }

        if (now - last_report >= kReportMs * 1000) {
            if (worst_us > worst_ever_us) worst_ever_us = worst_us;
            ESP_LOGI(kTag,
                     "%" PRIu32 " ticks: <50us %" PRIu32 ", <200 %" PRIu32
                     ", <500 %" PRIu32 ", <1ms %" PRIu32 ", <3ms %" PRIu32
                     ", >=3ms %" PRIu32 "; worst %" PRId64 " us (ever %" PRId64 ")",
                     ticks, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
                     worst_us, worst_ever_us);
            for (auto& h : hist) h = 0;
            ticks = 0;
            worst_us = 0;
            last_report = now;
        }
    }
}

}  // namespace

void start_ctrl_probe(services::Deps& d) {
    xTaskCreatePinnedToCore(probe_task, "ctrl", 4096, &d, 18, nullptr, 0);
}

}  // namespace demo
