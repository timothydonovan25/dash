// `logger` — core 1, priority 5.
//
// Drains the blackbox ring to the card in big batches. It is on core 1, at a
// low priority, behind a ring buffer, for one reason: an SD card can stall
// for hundreds of milliseconds without warning, and nothing on core 0 may
// ever wait on that. A dropped record is a cost the ring counts; a late
// shift light is not a cost at all, it is a bug.
#include "services/services.hpp"
#include "core/log/log.hpp"
#include "drivers/sdcard.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

namespace services {
namespace {

// 8 kB per write, as §9.2 says. An SD card charges roughly the same latency
// for 512 bytes as for 8 kB, so the batch size is free throughput.
constexpr size_t kBlockBytes = 8192;
constexpr size_t kBatch = 64;

void logger_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    char     block[kBlockBytes];
    size_t   used = 0;
    char     path[32] = {};
    bool     header_written = false;
    uint32_t written_records = 0;

    for (;;) {
        auto* card = static_cast<drivers::SdCard*>(d.card);
        if (!d.blackbox || !card || !card->present()) {
            // No card, or it has been pulled. Keep draining the ring so the
            // producer never wedges, and throw the records away — a dropped
            // log is a nuisance, a stalled `logic` task is not.
            if (d.blackbox) d.blackbox->clear();
            used = 0;
            header_written = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (path[0] == '\0') {
            // One file per power-on. There is no real-time clock on this
            // board and the GPS may not have a fix yet, so the name is a
            // session counter rather than a date — sortable, and it never
            // collides with a file already on the card.
            for (int i = 0; i < 1000; ++i) {
                std::snprintf(path, sizeof path, "logs/s%03d.csv", i);
                char full[64];
                std::snprintf(full, sizeof full, "/sd/%s", path);
                FILE* f = std::fopen(full, "rb");
                if (!f) break;              // this one is free
                std::fclose(f);
                path[0] = '\0';
            }
            if (path[0] == '\0') {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        if (!header_written) {
            char head[256];
            const size_t n = core::log::csv_header(head, sizeof head);
            if (hal::ok(card->append(path, head, n))) header_written = true;
        }

        core::log::Record batch[kBatch];
        const size_t n = d.blackbox->pop_batch(batch, kBatch);

        for (size_t i = 0; i < n; ++i) {
            char line[256];
            size_t len = 0;
            // A lap tag goes in immediately before the row it happened on.
            // RaceRender takes them anywhere in the file but insists they be
            // in chronological order, and this is the only place that sees
            // the records in order.
            if (batch[i].lap_done_ms != 0) {
                char tag[64];
                const size_t t = core::log::format_lap_marker(
                    tag, sizeof tag, batch[i].lap_num, batch[i].lap_done_ms);
                for (size_t k = 0; k < t && len + 1 < sizeof line; ++k) {
                    line[len++] = tag[k];
                }
            }
            len += core::log::format_csv(line + len, sizeof line - len, batch[i]);
            if (used + len >= sizeof block) {
                const hal::Status s = card->append(path, block, used);
                used = 0;
                if (hal::ok(s)) {
                    d.health->report_ok(core::state::Dev::Sd, d.clock->now_ms());
                } else {
                    d.health->report_error(core::state::Dev::Sd,
                                           static_cast<int32_t>(s),
                                           d.clock->now_ms());
                }
            }
            for (size_t k = 0; k < len; ++k) block[used++] = line[k];
            ++written_records;
        }

        if (d.system) {
            d.system->log_records = written_records;
            d.system->log_dropped = d.blackbox->dropped();
        }

        if (n == 0) {
            // Idle: push out whatever is buffered so a log never lags the
            // ride by more than a moment, then sleep.
            if (used > 0) {
                card->append(path, block, used);
                used = 0;
            }
            card->sync();
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

}  // namespace

void start_logger(Deps& d) {
    xTaskCreatePinnedToCore(logger_task, "logger", 6144, &d, 5, nullptr, 1);
}

}  // namespace services
