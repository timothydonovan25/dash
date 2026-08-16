// `storage` — core 1, priority 3.
//
// Owns the card: mounts it, notices it being pulled, remounts it when it
// comes back, and is the one place that would hand it to a host over USB.
// The logger writes through it rather than around it, so there is exactly
// one answer to "who has the card right now".
//
// USB mass storage is NOT implemented (§9.3). It is a real constraint rather
// than a missing function: the ESP32-S3 can present the card to a laptop
// over its native USB, but only if nothing on the device has it mounted at
// the same time — two writers on one FAT volume corrupts it. So the hand-off
// has to unmount, stop the logger, and refuse to log until the host lets go.
// That state machine is here; the TinyUSB half of it is not, and pretending
// otherwise would be worse than a clear gap.
#include "services/services.hpp"
#include "drivers/mcp23s17.hpp"
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

    uint32_t last_attempt_ms = 0;
    bool was_present = false;

    for (;;) {
        const uint32_t now = d.clock->now_ms();
        auto* card = static_cast<drivers::SdCard*>(d.card);
        if (!card) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Card detect lives on the expander (GPB7), not on a GPIO, so it
        // arrives second-hand from whoever last polled it.
        bool inserted = true;
        if (auto* mcp = static_cast<drivers::Mcp23s17*>(d.board)) {
            inserted = mcp->card_detect();
        }
        card->note_card_detect(inserted);

        if (!inserted) {
            if (was_present) {
                // Pulled. Unmount so the FAT structures are not left half
                // written, and let the logger discover there is nowhere to
                // write rather than telling it.
                card->unmount();
                was_present = false;
                d.health->set_absent(core::state::Dev::Sd);
            }
        } else if (!card->present()) {
            if (now - last_attempt_ms >= kRemountAfterMs) {
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
            d.health->report_ok(core::state::Dev::Sd, now);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

void start_storage(Deps& d) {
    xTaskCreatePinnedToCore(storage_task, "storage", 4096, &d, 3, nullptr, 1);
}

}  // namespace services
