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
//
// It also drains the detail log (core/log/detail.hpp) into detail/: four
// more rings, one per producer, and a fifth file it writes itself, the 1 Hz
// diag row, read off the slots and SystemInfo. Each of the six files has its
// own RAM block and its own FlushPolicy; one sync commits them all. The
// card's own write times go into the diag row, because every ring's size
// rests on a guess at how long a card can stall.
#include "services/services.hpp"
#include "core/log/detail.hpp"
#include "core/log/log.hpp"
#include "bsp/pins.hpp"
#include "drivers/sdcard.hpp"

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace services {
namespace {

using core::log::Stream;
using core::log::kStreamCount;

// 8 kB per write, as §9.2 says. An SD card charges roughly the same latency
// for 512 bytes as for 8 kB, so the batch size is free throughput. The raw
// IMU file is the fastest at ~12 kB/s. Its block was 16 kB until 1.61, but
// the SD host holds SPI2 for a whole multi-block write, busy time included,
// and the IMU's FIFO (~128 samples, ~680 ms) waits behind it: an hour's soak
// at 16 kB had 4 wakes over 100 samples behind, at 8 kB none (bringup §12).
constexpr size_t kBlockBytes = 8192;
constexpr size_t kImuBlockBytes = 8192;
constexpr size_t kSmallBlockBytes = 4096;   // events, diag
constexpr size_t kBatch = 64;

// A write or sync this slow counts in the diag row's SdSlow.
constexpr uint32_t kSlowMs = 100;
constexpr uint32_t kDiagEveryMs = 1000;

struct Out {
    char*    block = nullptr;
    size_t   cap = 0;
    size_t   used = 0;
    char     path[48] = {};
    bool     header = false;      // its header is in the block or on the card
    bool     on_card = false;     // something has been written under `path`
    uint32_t rows = 0;
    core::log::FlushPolicy flush;
};

// What each drain pops into, a batch at a time.
struct Batches {
    core::log::Record   main[kBatch];
    core::log::ImuRow   imu[kBatch];
    core::log::TachRow  tach[kBatch];
    core::log::DetRow   det[kBatch];
    core::log::EventRow evt[kBatch];
};

struct Logger {
    Deps*            d = nullptr;
    drivers::SdCard* card = nullptr;
    Out              out[kStreamCount];
    int              session = -1;
    bool             dated = false;
    core::log::Date  date;            // year 0 until the GPS gives one
    uint32_t         next_diag_ms = 0;
    // The card, this diag second.
    uint32_t         sd_bytes = 0;
    uint32_t         sd_max_ms = 0;
    uint16_t         sd_slow = 0;
    uint16_t         sd_errors = 0;
    // shortcut: bring-up totals for the console; remove once DIAG is on a panel.
    uint32_t         tot_bytes = 0, tot_errors = 0, tot_max_ms = 0, next_report_ms = 0;
    char             line[1024] = {};
    Batches*         batch = nullptr;
};

uint32_t now_ms(const Logger& L) { return L.d->clock->now_ms(); }

// The card's time for one call, into this second's figures.
void account(Logger& L, uint32_t t0, hal::Status s, size_t bytes) {
    const uint32_t dt = now_ms(L) - t0;
    if (dt > L.sd_max_ms) L.sd_max_ms = dt;
    if (dt > L.tot_max_ms) L.tot_max_ms = dt;
    if (dt >= kSlowMs && L.sd_slow < UINT16_MAX) ++L.sd_slow;
    if (hal::ok(s)) {
        L.sd_bytes += static_cast<uint32_t>(bytes);
        L.tot_bytes += static_cast<uint32_t>(bytes);
    } else if (s != hal::Status::Busy) {
        if (L.sd_errors < UINT16_MAX) ++L.sd_errors;
        ++L.tot_errors;
        ESP_LOGW("logger", "card write failed: %s", hal::status_name(s));
    }
}

hal::Status append(Logger& L, Out& o) {
    const uint32_t t0 = now_ms(L);
    const hal::Status s = L.card->append(o.path, o.block, o.used);
    account(L, t0, s, o.used);
    return s;
}

hal::Status sync_all(Logger& L) {
    const uint32_t t0 = now_ms(L);
    const hal::Status s = L.card->sync();
    account(L, t0, s, 0);
    return s;
}

void report(Logger& L, hal::Status s) {
    if (hal::ok(s)) {
        L.d->health->report_ok(core::state::Dev::Sd, now_ms(L));
    } else {
        L.d->health->report_error(core::state::Dev::Sd, static_cast<int32_t>(s), now_ms(L));
    }
}

// The block goes to the card because it is full. Busy is another task
// holding the card lock past the 3 s wait, not a dead card: try again rather
// than throw a block of ride away. The rings hold ten seconds meanwhile.
void write_full(Logger& L, Out& o) {
    if (o.used == 0) return;
    hal::Status s = append(L, o);
    for (int retry = 0; s == hal::Status::Busy && retry < 2; ++retry) s = append(L, o);
    o.used = 0;
    o.flush.written(now_ms(L));
    if (hal::ok(s)) o.on_card = true;
    report(L, s);
}

// Text into a stream's block, sending the block first if it would not fit.
void emit(Logger& L, Out& o, const char* text, size_t len) {
    if (len >= o.cap) len = o.cap - 1;
    if (o.used + len >= o.cap) write_full(L, o);
    std::memcpy(o.block + o.used, text, len);
    o.used += len;
    o.flush.buffered(now_ms(L));
}

void emit_str(Logger& L, Out& o, const char* text) { emit(L, o, text, std::strlen(text)); }

const char* reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "other";
    }
}

// What every detail file starts with: which firmware, which session, and
// how this power-on began. A brownout reset on the bike is the battery or
// the wiring, and it should be in the file rather than in a guess.
void detail_preamble(Logger& L, Out& o, Stream s) {
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_reset_reason_t rr = esp_reset_reason();
    std::snprintf(L.line, sizeof L.line,
                  "# dash detail log: %s\n# firmware %s %s\n# session %d, reset %s (%d)\n",
                  core::log::stream_suffix(s), app->version, app->date, L.session,
                  reset_name(rr), static_cast<int>(rr));
    emit_str(L, o, L.line);
}

// The events file also carries every setting as it stood at key-on, and the
// numbers baked in rather than set: an edit later is a `cmd` event.
void settings_preamble(Logger& L, Out& o) {
    const core::config::Settings& st = *L.d->settings;
    std::snprintf(L.line, sizeof L.line, "# tach refractory %d us\n", bsp::kTachRefractoryUs);
    emit_str(L, o, L.line);
    for (size_t i = 0; i < core::config::item_count(); ++i) {
        const core::config::ConfigItem& it = core::config::items()[i];
        if (it.kind == core::config::Kind::Action) continue;
        char value[32];
        core::config::format(st, it, value, sizeof value);
        std::snprintf(L.line, sizeof L.line, "# setting %s/%s = %s [0x%04X]\n",
                      it.group, it.label, value, static_cast<unsigned>(it.key));
        emit_str(L, o, L.line);
    }
}

void write_header(Logger& L, Stream s) {
    Out& o = L.out[s];
    size_t n = 0;
    switch (s) {
        case core::log::kMain:
            // The RaceRender marker has to be the very first line.
            n = core::log::csv_header(L.line, sizeof L.line);
            emit(L, o, L.line, n);
            o.header = true;
            return;
        case core::log::kImu:  detail_preamble(L, o, s); n = core::log::imu_header(L.line, sizeof L.line); break;
        case core::log::kTach: detail_preamble(L, o, s); n = core::log::tach_header(L.line, sizeof L.line); break;
        case core::log::kDet:  detail_preamble(L, o, s); n = core::log::det_header(L.line, sizeof L.line); break;
        case core::log::kEvt:
            detail_preamble(L, o, s);
            settings_preamble(L, o);
            n = core::log::event_header(L.line, sizeof L.line);
            break;
        case core::log::kDiag: detail_preamble(L, o, s); n = core::log::diag_header(L.line, sizeof L.line); break;
        case kStreamCount: return;
    }
    emit(L, o, L.line, n);
    o.header = true;
}

void reset_streams(Logger& L) {
    for (Out& o : L.out) {
        o.used = 0;
        o.header = false;
        o.on_card = false;
        o.path[0] = '\0';
        o.flush = core::log::FlushPolicy{};
    }
    L.session = -1;
    L.dated = false;
    L.date = core::log::Date{};
}

void clear_detail_rings(Deps& d) {
    if (d.imu_log) d.imu_log->clear();
    if (d.tach_log) d.tach_log->clear();
    if (d.det_log) d.det_log->clear();
    if (d.evt_log) d.evt_log->clear();
}

// One file set per power-on (or card insertion). There is no real-time clock
// on this board and the GPS may not have the date yet, so it starts as
// sNNN and is renamed to the date once it is known (core::log::
// log_file_name). The session number is one past the highest on the card in
// either form and in either directory: a renamed file frees its plain name,
// a second ride on the same day must not land on the first one's dated name,
// and a blackbox deleted from the Wi-Fi page may leave its detail behind.
bool pick_session(Logger& L) {
    int highest = -1;
    const auto take = [](const char* name, size_t, void* ctx) {
        int& h = *static_cast<int*>(ctx);
        const int v = core::log::session_of(name);
        if (v > h) h = v;
    };
    L.card->list("logs", take, &highest);
    L.card->list(core::log::kDetailDir, take, &highest);
    const int session = (highest + 1) % 1000;
    for (uint8_t s = 0; s < kStreamCount; ++s) {
        Out& o = L.out[s];
        if (core::log::stream_file_name(o.path, sizeof o.path, core::log::Date{},
                                        static_cast<unsigned>(session),
                                        static_cast<Stream>(s)) == 0) {
            return false;
        }
    }
    if (L.card->exists(L.out[core::log::kMain].path)) return false;
    L.session = session;
    return true;
}

// The day, once the GPS has said it. Whatever is buffered goes out under the
// old name first, so nothing is written to a name that is about to stop
// existing. Once, success or not: a rename that failed (a name taken, a card
// error) keeps the session name rather than retrying every pass.
void maybe_date(Logger& L) {
    if (L.dated) return;
    const core::state::Gnss g = L.d->slots->gnss.get();
    if (g.year == 0) return;
    core::log::Date utc;
    utc.year = g.year;
    utc.month = g.month;
    utc.day = g.day;
    L.date = core::log::local_date(utc, g.utc_ms,
                                   L.d->settings ? L.d->settings->tz_offset_h : 0);
    for (uint8_t s = 0; s < kStreamCount; ++s) {
        Out& o = L.out[s];
        char dated[sizeof o.path];
        if (core::log::stream_file_name(dated, sizeof dated, L.date,
                                        static_cast<unsigned>(L.session),
                                        static_cast<Stream>(s)) == 0) {
            continue;
        }
        if (!o.on_card) {
            // Nothing on the card yet: just start under the date.
            if (!L.card->exists(dated)) std::snprintf(o.path, sizeof o.path, "%s", dated);
            continue;
        }
        if (o.used > 0 && hal::ok(append(L, o))) {
            o.used = 0;
            o.flush.written(now_ms(L));
        }
        if (o.used == 0 && hal::ok(L.card->rename(o.path, dated))) {
            std::snprintf(o.path, sizeof o.path, "%s", dated);
        }
    }
    L.dated = true;
}

size_t drain_main(Logger& L) {
    Deps& d = *L.d;
    Out& o = L.out[core::log::kMain];
    if (!o.header) write_header(L, core::log::kMain);
    core::log::Record* rows = L.batch->main;
    const size_t n = d.blackbox->pop_batch(rows, kBatch);
    for (size_t i = 0; i < n; ++i) {
        size_t len = 0;
        // A lap tag goes in immediately before the row it happened on.
        // RaceRender takes them anywhere in the file but insists they be
        // in chronological order, and this is the only place that sees
        // the records in order.
        if (rows[i].lap_done_ms != 0) {
            len = core::log::format_lap_marker(L.line, sizeof L.line,
                                               rows[i].lap_num,
                                               rows[i].lap_done_ms);
        }
        len += core::log::format_csv(L.line + len, sizeof L.line - len, rows[i]);
        emit(L, o, L.line, len);
        ++o.rows;
    }
    return n;
}

template <typename Row, size_t N>
size_t drain(Logger& L, Stream s, core::log::Ring<Row, N>* ring, Row* batch,
             size_t (*format)(char*, size_t, const Row&)) {
    if (!ring) return 0;
    Out& o = L.out[s];
    if (!o.header) write_header(L, s);
    const size_t n = ring->pop_batch(batch, kBatch);
    for (size_t i = 0; i < n; ++i) {
        const size_t len = format(L.line, sizeof L.line, batch[i]);
        emit(L, o, L.line, len);
        ++o.rows;
    }
    return n;
}

// The machine, once a second: read, not produced, so `logger` is its one
// writer and it needs no ring.
void write_diag(Logger& L) {
    Deps& d = *L.d;
    Out& o = L.out[core::log::kDiag];
    if (!o.header) write_header(L, core::log::kDiag);
    EXT_RAM_BSS_ATTR static core::log::DiagRow r;   // ~0.2 kB; one logger task
    r = core::log::DiagRow{};
    r.t_ms = now_ms(L);
    if (const ui::SystemInfo* si = d.system) {
        r.heap_free_b = si->heap_free_b;
        r.heap_min_b = si->heap_min_b;
        r.psram_free_b = si->psram_free_b;
        std::memcpy(r.stack_low_task, si->stack_low_task, sizeof r.stack_low_task);
        r.stack_low_task[sizeof r.stack_low_task - 1] = '\0';
        r.stack_low_free_b = si->stack_low_free_b;
        r.ctrl_late = si->ctrl_late;
        r.render_hz_x10 = si->render_hz_x10;
        r.imu_timeouts = si->imu_timeouts;
        r.gnss_sentences = si->gnss_sentences;
        r.gnss_bad_csum = si->gnss_bad_csum;
        r.pod_recoveries = si->pod_recoveries;
        r.pod_stuck = si->pod_stuck;
        r.chip_temp_c = si->chip_temp_c;
        r.chip_temp_ok = si->chip_temp_ok;
    }
    r.imu_temp_c = d.slots->attitude.get().imu_temp_c;
    r.vbat = d.slots->elec.get().vbat;
    const core::state::Battery bat = d.slots->battery.get();
    r.rest_v = bat.rest_v;
    r.crank_v = bat.crank_min_v;
    r.charge_v = bat.charge_v;
    r.battery_verdict = bat.verdict;
    const core::state::Gnss g = d.slots->gnss.get();
    r.fix = g.fix;
    r.sats = g.sats;
    r.quality = g.quality;
    r.hdop = g.hdop;
    const core::state::Trip t = d.slots->trip.get();
    r.odo_mi = t.odo_mi;
    r.eng_hours = t.eng_hours;
    r.fuel_used_gal = t.fuel_used_gal;
    for (uint8_t s = 0; s < kStreamCount; ++s) r.rows[s] = L.out[s].rows;
    r.dropped[core::log::kMain] = d.blackbox ? d.blackbox->dropped() : 0;
    r.dropped[core::log::kImu] = d.imu_log ? d.imu_log->dropped() : 0;
    r.dropped[core::log::kTach] = d.tach_log ? d.tach_log->dropped() : 0;
    r.dropped[core::log::kDet] = d.det_log ? d.det_log->dropped() : 0;
    r.dropped[core::log::kEvt] = d.evt_log ? d.evt_log->dropped() : 0;
    r.sd_bytes = L.sd_bytes;
    r.sd_max_ms = L.sd_max_ms;
    r.sd_slow = L.sd_slow;
    r.sd_errors = L.sd_errors;
    L.sd_bytes = 0;
    L.sd_max_ms = 0;
    L.sd_slow = 0;
    L.sd_errors = 0;
    const core::state::Health h = d.slots->health.get();
    for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
        r.dev_state[i] = static_cast<uint8_t>(h.dev[i].state);
        r.dev_faults[i] = h.dev[i].fault_count;
    }
    const core::state::BleDiag bd = d.slots->ble_diag.get();
    r.rssi_dbm = bd.has_rssi ? bd.rssi_dbm : 0;
    r.wifi_state = static_cast<uint8_t>(d.slots->wifi.get().state);
    const size_t n = core::log::format_diag(L.line, sizeof L.line, r);
    emit(L, o, L.line, n);
    ++o.rows;
}

// When each block goes to the card and when the files are committed
// (core::log::FlushPolicy, one per file): a block when its oldest row is 2 s
// old, all of them committed 5 s after the first uncommitted write or when
// the bike stops. One sync commits every open file, so it is done once.
void flush(Logger& L) {
    const uint32_t now = now_ms(L);
    const bool stopped = still_for_flash(*L.d);
    const core::log::FlushConfig cfg;
    bool want_sync = false;
    for (Out& o : L.out) {
        const core::log::Flush f = o.flush.decide(now, stopped, cfg);
        if (f != core::log::Flush::None && o.used > 0) {
            // Kept for the next pass if the card was only busy.
            const hal::Status st = append(L, o);
            if (st != hal::Status::Busy) {
                o.used = 0;
                o.flush.written(now);
                if (hal::ok(st)) o.on_card = true;
                else report(L, st);
            }
        }
        if (f == core::log::Flush::WriteAndSync && o.used == 0) want_sync = true;
    }
    if (!want_sync) return;
    // Busy is another task holding the card lock past its 3 s wait, and the
    // next pass tries again. Anything else, failed or not, waits for the
    // next interval: a card that will not sync is not retried every pass
    // (rule 5).
    const hal::Status st = sync_all(L);
    if (st == hal::Status::Busy) return;
    for (Out& o : L.out) {
        if (o.used == 0) o.flush.synced();
    }
    if (!hal::ok(st)) report(L, st);
}

void logger_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);
    // Static and in PSRAM, not on the stack: the blocks alone are bigger
    // than this task's stack, and none of it is in internal RAM's tight
    // .bss. One logger task, so these are its own; nothing here is
    // time-critical enough to care about PSRAM's latency.
    EXT_RAM_BSS_ATTR static Logger L;
    EXT_RAM_BSS_ATTR static Batches batches;
    EXT_RAM_BSS_ATTR static char main_block[kBlockBytes];
    EXT_RAM_BSS_ATTR static char imu_block[kImuBlockBytes];
    EXT_RAM_BSS_ATTR static char tach_block[kBlockBytes];
    EXT_RAM_BSS_ATTR static char det_block[kBlockBytes];
    EXT_RAM_BSS_ATTR static char evt_block[kSmallBlockBytes];
    EXT_RAM_BSS_ATTR static char diag_block[kSmallBlockBytes];
    L.d = &d;
    L.batch = &batches;
    const auto give = [](Out& o, char* block, size_t cap) { o.block = block; o.cap = cap; };
    give(L.out[core::log::kMain], main_block, sizeof main_block);
    give(L.out[core::log::kImu], imu_block, sizeof imu_block);
    give(L.out[core::log::kTach], tach_block, sizeof tach_block);
    give(L.out[core::log::kDet], det_block, sizeof det_block);
    give(L.out[core::log::kEvt], evt_block, sizeof evt_block);
    give(L.out[core::log::kDiag], diag_block, sizeof diag_block);
    reset_streams(L);

    for (;;) {
        // The supply has been cut and the board is on its capacitors. A
        // write started now would probably be the one that is interrupted,
        // and a half-written FAT cluster costs more than a few lost rows.
        // Nothing is popped, so the rings simply fill; if power returns the
        // drain carries on where it was.
        if (d.power_fail.load(std::memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        L.card = static_cast<drivers::SdCard*>(d.card);
        if (!d.blackbox || !L.card || !L.card->present()) {
            // No card, or it has been pulled. Keep draining the rings so the
            // producers never wedge, and throw the records away — a dropped
            // log is a nuisance, a stalled `logic` task is not.
            if (d.blackbox) d.blackbox->clear();
            clear_detail_rings(d);
            reset_streams(L);         // a card put back starts new files
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (L.session < 0 && !pick_session(L)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        maybe_date(L);

        const bool detail = d.settings && d.settings->detail_log;
        const auto drain_all = [&]() {
            size_t got = drain_main(L);
            if (detail) {
                got += drain(L, core::log::kImu, d.imu_log, batches.imu, core::log::format_imu);
                got += drain(L, core::log::kTach, d.tach_log, batches.tach, core::log::format_tach);
                got += drain(L, core::log::kDet, d.det_log, batches.det, core::log::format_det);
                got += drain(L, core::log::kEvt, d.evt_log, batches.evt, core::log::format_event);
            }
            return got;
        };
        size_t n = drain_all();
        if (detail) {
            const uint32_t now = now_ms(L);
            if (static_cast<int32_t>(now - L.next_diag_ms) >= 0) {
                L.next_diag_ms = now + kDiagEveryMs;
                write_diag(L);
            }
        } else {
            // Turned off: the producers stop pushing, and whatever was in
            // flight goes. The files stay as they are; turned on again, the
            // same files carry on.
            clear_detail_rings(d);
        }

        // Eject asked for everything on the card before it unmounts: the
        // rings (bounded: they refill while this runs), every block, a sync.
        const uint32_t req = d.log_flush_req.load(std::memory_order_acquire);
        if (req != d.log_flush_ack.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 8 && n > 0; ++i) n = drain_all();
            const uint32_t now = now_ms(L);
            size_t bytes = 0;
            for (Out& o : L.out) {
                if (o.used == 0) continue;
                bytes += o.used;
                const hal::Status st = append(L, o);
                o.used = 0;
                o.flush.written(now);
                if (hal::ok(st)) o.on_card = true;
                else report(L, st);
            }
            const hal::Status st = sync_all(L);
            for (Out& o : L.out) o.flush.synced();
            if (!hal::ok(st)) report(L, st);
            ESP_LOGI("logger", "flushed for eject: %u bytes, sync %s",
                     static_cast<unsigned>(bytes), hal::status_name(st));
            d.log_flush_ack.store(req, std::memory_order_release);
        }

        if (d.system) {
            d.system->log_records = L.out[core::log::kMain].rows;
            d.system->log_dropped = d.blackbox->dropped();
        }

        flush(L);
        if (static_cast<int32_t>(now_ms(L) - L.next_report_ms) >= 0) {
            L.next_report_ms = now_ms(L) + 10000;
            ESP_LOGI("logger", "%s: %u rows, %u bytes to the card, %u errors, slowest %u ms",
                     L.out[core::log::kMain].path, static_cast<unsigned>(L.out[core::log::kMain].rows),
                     static_cast<unsigned>(L.tot_bytes), static_cast<unsigned>(L.tot_errors),
                     static_cast<unsigned>(L.tot_max_ms));
        }
        vTaskDelay(pdMS_TO_TICKS(n == 0 ? 200 : 20));
    }
}

}  // namespace

void start_logger(Deps& d) {
    xTaskCreatePinnedToCore(logger_task, "logger", 6144, &d, 2, nullptr, 1);
}

}  // namespace services
