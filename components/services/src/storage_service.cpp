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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

// Long enough that a card jiggling in its socket over a whoop section does
// not remount four times.
constexpr uint32_t kRemountAfterMs = 2000;

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

    for (;;) {
        const uint32_t now = d.clock->now_ms();
        auto* card = static_cast<drivers::SdCard*>(d.card);
        if (!card) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const bool inserted = card->card_detect();

        const uint32_t gen = d.slots->storage_request.generation();
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

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

void start_storage(Deps& d) {
    xTaskCreatePinnedToCore(storage_task, "storage", 4096, &d, 3, nullptr, 1);
}

}  // namespace services
