// `storage` — core 1, priority 3.
//
// Owns the card: mounts it, notices it being pulled, remounts it when it
// comes back, and is the one place that would hand it to a host over USB.
// The logger writes through it rather than around it, so there is exactly
// one answer to "who has the card right now".
//
// USB mass storage is NOT implemented (§9.3). It is a real constraint rather
// than a missing function: the ESP32-P4 can present the card to a laptop
// over its native USB, but only if nothing on the device has it mounted at
// the same time — two writers on one FAT volume corrupts it. So the hand-off
// has to unmount, stop the logger, and refuse to log until the host lets go.
// That state machine is here; the TinyUSB half of it is not, and pretending
// otherwise would be worse than a clear gap.
#include "services/services.hpp"
#include "drivers/sdcard.hpp"
#include "drivers/usb_drive.hpp"
#include <initializer_list>

#include "esp_log.h"
#include "esp_system.h"
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr const char* kTag = "storage";

// Set by the console's `route bench`, taken by this task.
std::atomic<bool> g_bench{false};

// Two fixed ends across the state, routed into the buffer `logic` is not
// following (it never reads that one), cold and then warm.
void run_bench(Deps& d, hal::ICard& card) {
    RouteMailbox& box = *d.route;
    RouteEngine& eng = *d.route_engine;
    if (!box.free()) { ESP_LOGW(kTag, "route bench: a routing job is out; try again"); return; }
    size_t size = 0;
    if (!hal::ok(card.size_of(kMapPath, size))) { ESP_LOGW(kTag, "route bench: no %s", kMapPath); return; }
    const bool reopen = !eng.graph.is_open() || eng.src.size() != size;
    eng.src.set(&card, static_cast<uint32_t>(size));   // this call's card (route_store's ensure_open)
    if (reopen) {
        const core::route::OpenResult o = eng.graph.open(eng.src);
        if (o != core::route::OpenResult::Ok) {
            ESP_LOGW(kTag, "route bench: map: %s", core::route::to_string(o));
            return;
        }
    }
    RouteData& out = box.data[box.live == 0 ? 1 : 0];
    const core::route::Point a{360626000, -941574000}, b{347465000, -922896000};
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0) eng.cache.attach(&eng.src);          // cold: nothing cached
        core::route::QueryStats st;
        const uint32_t t0 = d.clock->now_ms();
        const core::route::Status s = core::route::query(eng.graph, 0, a, b, eng.search.mem(), out.route, &st);
        const unsigned tenths = static_cast<unsigned>(out.route.total_dm / 1609.344);
        ESP_LOGI(kTag, "route bench %s: %s in %u ms, %u.%u mi, %u settled, %u block reads, %u points",
                 pass ? "warm" : "cold", core::route::to_string(s),
                 static_cast<unsigned>(d.clock->now_ms() - t0), tenths / 10, tenths % 10,
                 static_cast<unsigned>(st.settled), static_cast<unsigned>(st.cache_misses),
                 static_cast<unsigned>(out.route.n_pts));
    }
}

// Long enough that a card jiggling in its socket over a whoop section does
// not remount four times.
constexpr uint32_t kRemountAfterMs = 2000;

// After a USB drive session the dash restarts (parked; ~2 s). The USB
// serial driver did not survive its pads going off and on under it: the
// console came back enumerated but silent (seen on the bike, 2026-11-05).
// A boot brings it and the card's mount back clean (UsbDrive::restore_phy).
// The card as route and search jobs see it, each good read counted as the
// card working. A route is seconds of reads in which this loop does not come
// round to its keepalive below, and SD went stale and then Fault: LOG
// STOPPED over a card that was being read fine (2.6).
class ReadsKeepAlive final : public hal::ICard {
public:
    ReadsKeepAlive(hal::ICard& c, Deps& d) : c_(c), d_(d) {}
    hal::Status init() override { return c_.init(); }
    bool present() const override { return c_.present(); }
    hal::Status append(const char* p, const void* b, size_t n) override { return c_.append(p, b, n); }
    hal::Status sync() override { return c_.sync(); }
    hal::Status size_of(const char* p, size_t& out) override { return c_.size_of(p, out); }
    hal::Status read_at(const char* p, size_t off, void* b, size_t n, size_t& got) override {
        const hal::Status s = c_.read_at(p, off, b, n, got);
        // Only the logger's next good write ends a streak of failed ones.
        if (hal::ok(s) && !d_.health->erring(core::state::Dev::Sd))
            d_.health->report_ok(core::state::Dev::Sd, d_.clock->now_ms());
        return s;
    }
    hal::Status list(const char* dir, ListFn fn, void* ctx, bool sizes) override {
        return c_.list(dir, fn, ctx, sizes);
    }
    hal::Status remove(const char* p) override { return c_.remove(p); }
    hal::Status write_file(const char* p, const void* b, size_t n) override { return c_.write_file(p, b, n); }
    hal::Status rename(const char* a, const char* b) override { return c_.rename(a, b); }
    hal::Status write_at(const char* p, size_t off, const void* b, size_t n) override {
        return c_.write_at(p, off, b, n);
    }

private:
    hal::ICard& c_;
    Deps& d_;
};

[[noreturn]] void restart_after_drive(const char* why) {
    ESP_LOGW(kTag, "USB drive off (%s): restarting to give the dash its card and console back", why);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void storage_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    // The first mount is this task's too, not the boot's: a slow card would
    // otherwise hold back the first frame. So the first attempt does not
    // wait out kRemountAfterMs.
    uint32_t last_attempt_ms = 0;
    bool attempted = false;
    bool was_present = false;
    // CONFIG's Eject unmounted the card and it has not come out yet. The
    // card is still in the socket, so without this the next pass would
    // mount it straight back.
    bool ejected = false;
    uint32_t request_gen = d.slots->storage_request.generation();
    // The card as the laptop's USB drive (drivers::UsbDrive): while it is,
    // nothing here mounts it or serves from it.
    drivers::UsbDrive drive;
    bool host_seen = false;
    uint32_t host_last_ms = 0, drive_since_ms = 0;

    for (;;) {
        const uint32_t now = d.clock->now_ms();
        auto* card = static_cast<drivers::SdCard*>(d.card);
        if (!card) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const bool inserted = card->card_detect();

        const uint32_t gen = d.slots->storage_request.generation();
        if (gen != request_gen && d.slots->storage_request.get().op ==
                                      core::state::StorageRequest::UsbDrive) {
            request_gen = gen;
            if (drive.active()) {
                // Asked again: the card back to the dash.
                drive.stop();
                restart_after_drive("asked");
            } else if (inserted) {
                // As Eject: the logger's blocks out first, then unmount.
                const uint32_t req = d.log_flush_req.fetch_add(1) + 1;
                for (int i = 0; i < 150 &&
                     d.log_flush_ack.load(std::memory_order_acquire) != req; ++i) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                if (card->unmount()) {
                    was_present = false;
                    d.health->set_absent(core::state::Dev::Sd);
                    ESP_LOGI(kTag, "USB drive on: the console goes quiet until it is off");
                    vTaskDelay(pdMS_TO_TICKS(100));      // that line out before the swap
                    if (hal::ok(drive.start())) {
                        host_seen = false;
                        drive_since_ms = now;
                    } else {
                        ESP_LOGW(kTag, "USB drive would not start; the card stays the dash's");
                    }
                }
            }
            if (d.system) d.system->sd_usb = drive.active();
        }
        if (drive.active()) {
            // Back to the dash when the laptop lets go (unplugged, or the
            // drive gone from it for 3 s), when nobody took it in a minute,
            // or when the card comes out.
            const bool host = drive.host_connected();
            if (host) { host_seen = true; host_last_ms = now; }
            const bool gone = host_seen && !host && now - host_last_ms >= 3000;
            const bool unclaimed = !host_seen && now - drive_since_ms >= 60000;
            if (gone || unclaimed || !inserted) {
                drive.stop();
                restart_after_drive(gone ? "laptop let go" : unclaimed ? "nobody took it" : "card out");
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }
        if (gen != request_gen) {
            request_gen = gen;
            if (d.slots->storage_request.get().op ==
                    core::state::StorageRequest::Eject && inserted) {
                // The logger's RAM blocks first (up to ~2 s of every
                // stream), then closing the files flushes them. The logger
                // sees the card gone on its next pass and stops writing.
                // Waited for, but not forever: a logger that does not answer
                // in 3 s (a dying card) costs those rows, not the eject.
                const uint32_t req = d.log_flush_req.fetch_add(1) + 1;
                for (int i = 0; i < 150 &&
                     d.log_flush_ack.load(std::memory_order_acquire) != req; ++i) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                // "Safe to pull" only once it really is unmounted; if the
                // card lock never came free, nothing changed and CONFIG's
                // Eject can go again.
                if (card->unmount()) {
                    was_present = false;
                    ejected = true;
                    d.health->set_absent(core::state::Dev::Sd);
                    if (d.system) d.system->sd_ejected = true;
                }
            }
        }

        if (!inserted) {
            if (ejected) {
                ejected = false;
                if (d.system) d.system->sd_ejected = false;
            }
            if (was_present) {
                // Pulled. Unmount so the FAT structures are not left half
                // written, and let the logger discover there is nowhere to
                // write rather than telling it.
                // Retried next pass if the lock did not come free.
                if (card->unmount()) {
                    was_present = false;
                    d.health->set_absent(core::state::Dev::Sd);
                }
            }
        } else if (ejected) {
            // Pulled and put back is the only way out of this.
        } else if (!card->present()) {
            if (!attempted || now - last_attempt_ms >= kRemountAfterMs) {
                attempted = true;
                last_attempt_ms = now;
                d.health->begin_init(core::state::Dev::Sd, now);
                const hal::Status s = card->init();
                if (hal::ok(s)) {
                    d.health->report_ok(core::state::Dev::Sd, now);
                    was_present = true;
                    // Uploads cut off by a reset leave their temporary files;
                    // none can be under way at mount, so they go.
                    for (const char* tmp : {"route/map.tmp", "route/search.tmp", "games/upload.tmp"})
                        card->remove(tmp);
                } else {
                    d.health->report_error(core::state::Dev::Sd,
                                           static_cast<int32_t>(s), now);
                }
            }
        } else {
            was_present = true;
            // Mounted says nothing about writes; only the logger's next
            // good write ends a streak of failed ones.
            if (!d.health->erring(core::state::Dev::Sd))
                d.health->report_ok(core::state::Dev::Sd, now);
        }

        // Saved tracks (services/track_store.hpp): one job per pass, while
        // the card is in and the supply holds. A save is one ~19 kB file, so
        // not one to start on the capacitors.
        if (d.tracks && card->present() && !ejected &&
            !d.power_fail.load(std::memory_order_relaxed)) {
            serve_tracks(*d.tracks, *card);
        }

        // Routing (services/route_store.hpp): a query is a second or two of
        // card reads, cold. Reads only, so the capacitors are no worry.
        if (d.route && d.route_engine && card->present() && !ejected &&
            g_bench.exchange(false)) {
            ReadsKeepAlive reads(*card, d);
            run_bench(d, reads);
        }
        if (d.route && d.route_engine && card->present() && !ejected) {
            const RouteMailbox::Op op = d.route->op;
            const uint32_t t0 = d.clock->now_ms();
            ReadsKeepAlive reads(*card, d);
            const bool served = serve_route(*d.route, *d.route_engine, reads);
            if (served && op == RouteMailbox::Op::Search) {
                // What Where to? found, for the console bench (navigation §7.1).
                ESP_LOGI(kTag, "search \"%s\": %s, %u matches, %u listed in %u ms%s%s",
                         d.route->query, d.route->search_ok ? "ok" : "no index",
                         static_cast<unsigned>(d.route->matches), static_cast<unsigned>(d.route->n_hits),
                         static_cast<unsigned>(d.clock->now_ms() - t0),
                         d.route->n_hits ? ", first: " : "", d.route->n_hits ? d.route->hits[0].name : "");
            }
            if (served && op == RouteMailbox::Op::Query) {
                // The bench number for docs/navigation.md §9: how long a
                // route takes through the card, and how many 4 kB reads.
                ESP_LOGI(kTag, "route: %s in %u ms, %u settled, %u block reads, %u points",
                         core::route::to_string(d.route->status),
                         static_cast<unsigned>(d.clock->now_ms() - t0),
                         static_cast<unsigned>(d.route->stats.settled),
                         static_cast<unsigned>(d.route->stats.cache_misses),
                         static_cast<unsigned>(d.route->data[d.route->into & 1].route.n_pts));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

void bench_route() { g_bench.store(true); }

void start_storage(Deps& d) {
    // 5 kB: the route query (core::route) runs on this stack, ~1 kB deep.
    // Priority 1, under the logger: a route is ~1,100 card reads back to
    // back, and above the logger it retook the card lock after every one,
    // holding the blackbox off past its 3 s lock wait (LOG STOPPED, 2.6).
    xTaskCreatePinnedToCore(storage_task, "storage", 5120, &d, 1, nullptr, 1);
}

}  // namespace services
