// `logger` — core 1, priority 2.
//
// Drains the blackbox ring to the card in big batches. It is on core 1, at a
// low priority, behind a ring buffer, for one reason: an SD card can stall
// for hundreds of milliseconds without warning, and nothing on core 0 may
// ever wait on that. A dropped record is a cost the ring counts; a late
// shift light is not a cost at all, it is a bug.
//
// Priority 2, below keeper, ble, storage and wifi: IDF's SD-over-SPI driver
// waits out a card's busy time by spinning, not sleeping, so whatever this
// task is above is starved for as long as the card is busy -- tens of ms,
// hundreds on a slow card -- and at 5 that was keeper's 20 Hz health table
// among others. The ring holds ten seconds; this only needs the average
// rate, which the idle time on core 1 has plenty of.
#include "services/services.hpp"
#include "core/log/log.hpp"
#include "drivers/sdcard.hpp"

#include "esp_attr.h"
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

    // Static and in PSRAM, not on the stack: the block alone is bigger than
    // this task's 6 kB stack, and the batch below is 6.6 kB more. One logger
    // task, so these are its own; nothing here is time-critical enough to
    // care about PSRAM's latency.
    EXT_RAM_BSS_ATTR static char block[kBlockBytes];
    EXT_RAM_BSS_ATTR static core::log::Record batch[kBatch];
    size_t   used = 0;
    char     path[40] = {};
    int      session = -1;
    bool     dated = false;
    bool     header_written = false;
    uint32_t written_records = 0;
    // When the block goes to the card and when the file is committed: not
    // every time the ring runs dry, which at 10 Hz was between every row.
    core::log::FlushPolicy flush;
    const core::log::FlushConfig flush_cfg;

    for (;;) {
        // The supply has been cut and the board is on its capacitors. A
        // write started now would probably be the one that is interrupted,
        // and a half-written FAT cluster costs more than a few lost rows.
        // Nothing is popped, so the ring simply fills; if power returns the
        // drain carries on where it was.
        if (d.power_fail.load(std::memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        auto* card = static_cast<drivers::SdCard*>(d.card);
        if (!d.blackbox || !card || !card->present()) {
            // No card, or it has been pulled. Keep draining the ring so the
            // producer never wedges, and throw the records away — a dropped
            // log is a nuisance, a stalled `logic` task is not.
            if (d.blackbox) d.blackbox->clear();
            used = 0;
            flush = core::log::FlushPolicy{};
            header_written = false;
            path[0] = '\0';           // a card put back starts a new file
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (path[0] == '\0') {
            // One file per power-on (or card insertion). There is no real-
            // time clock on this board and the GPS may not have the date
            // yet, so it starts as logs/sNNN.csv and is renamed to the date
            // below once it is known (core::log::log_file_name). The session
            // number is one past the highest on the card in either form: a
            // renamed file frees its plain name, and a second ride on the
            // same day must not land on the first one's dated name.
            int highest = -1;
            card->list("logs", [](const char* name, size_t, void* ctx) {
                int& h = *static_cast<int*>(ctx);
                const int v = core::log::session_of(name);
                if (v > h) h = v;
            }, &highest);
            session = (highest + 1) % 1000;
            dated = false;
            if (core::log::log_file_name(path, sizeof path, core::log::Date{},
                                         static_cast<unsigned>(session)) == 0 ||
                card->exists(path)) {
                path[0] = '\0';
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        // The day, once the GPS has said it. Whatever is buffered goes out
        // under the old name first, so nothing is written to a name that is
        // about to stop existing.
        if (!dated) {
            const core::state::Gnss g = d.slots->gnss.get();
            if (g.year != 0) {
                core::log::Date utc;
                utc.year = g.year;
                utc.month = g.month;
                utc.day = g.day;
                const core::log::Date local = core::log::local_date(
                    utc, g.utc_ms, d.settings ? d.settings->tz_offset_h : 0);
                char dated_path[sizeof path];
                if (core::log::log_file_name(dated_path, sizeof dated_path, local,
                                             static_cast<unsigned>(session)) > 0) {
                    if (!header_written) {
                        // Nothing on the card yet: just start under the date.
                        if (!card->exists(dated_path)) {
                            std::snprintf(path, sizeof path, "%s", dated_path);
                        }
                    } else {
                        if (used > 0 && hal::ok(card->append(path, block, used))) {
                            used = 0;
                            flush.written(d.clock->now_ms());
                        }
                        if (used == 0 && hal::ok(card->rename(path, dated_path))) {
                            std::snprintf(path, sizeof path, "%s", dated_path);
                        }
                    }
                }
                // Once, success or not: a rename that failed (a name taken,
                // a card error) keeps the session name rather than retrying
                // every pass.
                dated = true;
            }
        }

        if (!header_written) {
            char head[256];
            const size_t n = core::log::csv_header(head, sizeof head);
            if (hal::ok(card->append(path, head, n))) {
                header_written = true;
                flush.written(d.clock->now_ms());
            }
        }

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
                // Busy is another task holding the card lock past the 3 s
                // wait, not a dead card: try again rather than throw 8 kB of
                // ride away. The blackbox ring holds ten seconds meanwhile.
                hal::Status s = card->append(path, block, used);
                for (int retry = 0; s == hal::Status::Busy && retry < 2; ++retry) {
                    s = card->append(path, block, used);
                }
                used = 0;
                flush.written(d.clock->now_ms());
                if (hal::ok(s)) {
                    d.health->report_ok(core::state::Dev::Sd, d.clock->now_ms());
                } else {
                    d.health->report_error(core::state::Dev::Sd,
                                           static_cast<int32_t>(s),
                                           d.clock->now_ms());
                }
            }
            for (size_t k = 0; k < len; ++k) block[used++] = line[k];
            flush.buffered(d.clock->now_ms());
            ++written_records;
        }

        if (d.system) {
            d.system->log_records = written_records;
            d.system->log_dropped = d.blackbox->dropped();
        }

        // The block goes out when its oldest row is 2 s old, and the file is
        // committed 5 s after the first uncommitted write or when the bike
        // stops (core::log::FlushPolicy). This used to append and fsync
        // whenever the ring was empty -- at 10 Hz, between almost every row,
        // about 4.5 card writes a second each spinning through the card's
        // busy time on this core with SPI2 held from the IMU.
        const uint32_t now = d.clock->now_ms();
        const core::log::Flush f = flush.decide(now, still_for_flash(d), flush_cfg);
        if (f != core::log::Flush::None && used > 0) {
            // Kept for the next pass if the card was only busy.
            if (card->append(path, block, used) != hal::Status::Busy) {
                used = 0;
                flush.written(now);
            }
        }
        if (f == core::log::Flush::WriteAndSync && used == 0) {
            // Busy is another task holding the card lock past its 3 s wait,
            // and the next pass tries again. Anything else, failed or not,
            // waits for the next interval: a card that will not sync is not
            // retried every pass (rule 5).
            const hal::Status s = card->sync();
            if (s != hal::Status::Busy) {
                flush.synced();
                if (!hal::ok(s)) {
                    d.health->report_error(core::state::Dev::Sd, static_cast<int32_t>(s), now);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(n == 0 ? 200 : 20));
    }
}

}  // namespace

void start_logger(Deps& d) {
    xTaskCreatePinnedToCore(logger_task, "logger", 6144, &d, 2, nullptr, 1);
}

}  // namespace services
