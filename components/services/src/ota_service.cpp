// `ota` — core 1, priority 2, event driven (§11.7).
//
// Stable and dev firmware in the two app slots, and updating the dev one.
// The decisions are core::ota's, host-tested; this task does the flash
// calls and is the one writer of the `ota` slot.
//
// At boot it works out what the running image is, from the bootloader's own
// state rather than from anything stored (see core/ota/ota.hpp):
//   - stable: carries on. It is what every power-on boots: there is no
//     chooser, so nothing waits on the rider before the dash is up;
//   - dev: carries on and never confirms by itself, so the next reset is
//     stable again -- unless the rider promotes it from DIAG -> Update.
//
// Then it waits for the Update screen's requests, handed over by `logic`.
// Every request that writes flash is refused unless the bike is parked: an
// erase stops the flash cache on both cores, long enough to break the 3 ms
// tach-to-lamp budget on core 0.
#include "services/services.hpp"

#include "core/ota/ota.hpp"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace services {
namespace {

using core::ota::InstallError;
using core::ota::Install;
using core::ota::Role;
using core::ota::SlotState;

constexpr const char* kTag = "ota";

// Where the rider puts an update on the card.
constexpr const char* kUpdatePath = "update/dash.bin";

constexpr size_t   kChunk = 4096;
constexpr uint32_t kIdleWaitMs = 100;

// The chunk buffer. Static rather than on the stack, which stays small.
uint8_t g_chunk[kChunk];

SlotState state_of(const esp_partition_t* p) {
    esp_ota_img_states_t s;
    if (!p || esp_ota_get_state_partition(p, &s) != ESP_OK) return SlotState::None;
    switch (s) {
        case ESP_OTA_IMG_NEW:            return SlotState::New;
        case ESP_OTA_IMG_PENDING_VERIFY: return SlotState::PendingVerify;
        case ESP_OTA_IMG_VALID:          return SlotState::Valid;
        case ESP_OTA_IMG_INVALID:        return SlotState::Invalid;
        case ESP_OTA_IMG_ABORTED:        return SlotState::Aborted;
        default:                         return SlotState::None;
    }
}

void copy_str(char* dst, size_t cap, const char* src) {
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

// What the other slot holds, refreshed after anything that changes it.
void read_other(core::state::Ota& o, const esp_partition_t* other) {
    esp_app_desc_t desc;
    const bool has = other && esp_ota_get_partition_description(other, &desc) == ESP_OK;
    o.other = core::ota::other_from(has, state_of(other));
    if (has) copy_str(o.other_ver, sizeof o.other_ver, desc.version);
    else o.other_ver[0] = '\0';
}

class Ota {
public:
    explicit Ota(Deps& d) : d_(d) {}

    void boot() {
        running_ = esp_ota_get_running_partition();
        other_ = esp_ota_get_next_update_partition(running_);
        o_.known = true;
        o_.role = core::ota::role_of(state_of(running_));
        copy_str(o_.running_ver, sizeof o_.running_ver, esp_app_get_description()->version);
        copy_str(o_.running_slot, sizeof o_.running_slot, running_ ? running_->label : "?");
        read_other(o_, other_);
        ESP_LOGI(kTag, "running %s from %s, %s; other slot %s",
                 o_.running_ver, o_.running_slot, core::ota::role_name(o_.role),
                 o_.other == core::ota::Other::Ready ? o_.other_ver : "empty");
        publish();
    }

    void run() {
        uint32_t request_gen = d_.slots->ota_request.generation();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));

            const uint32_t gen = d_.slots->ota_request.generation();
            if (gen == request_gen) continue;
            request_gen = gen;
            switch (d_.slots->ota_request.get().op) {
                case core::state::OtaRequest::InstallSd:   install_sd(); break;
                case core::state::OtaRequest::BootDevOnce: boot_dev(); break;
                case core::state::OtaRequest::Promote:     promote(); break;
                default: break;
            }
        }
    }

private:
    void publish() { d_.slots->ota.publish(o_); }

    void fail(InstallError e) {
        o_.install = Install::Failed;
        o_.error = e;
        ESP_LOGW(kTag, "%s", core::ota::install_error_name(e));
        publish();
    }

    bool parked() const {
        const core::state::Motion m = d_.slots->motion.get();
        const core::state::Engine e = d_.slots->engine.get();
        return core::ota::may_write(m.best_mph, e.rpm);
    }

    // The rider's decision, made while dev is running: keep it. Confirming
    // the running image is the whole of it -- the bootloader then treats it
    // as the stable one, and the old stable becomes the slot the next
    // install writes. No restart.
    void promote() {
        if (o_.role != Role::Dev) return fail(InstallError::NotDev);
        if (!parked()) return fail(InstallError::Moving);
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
            return fail(InstallError::FlashFailed);
        }
        o_.role = Role::Stable;
        o_.install = Install::Idle;
        o_.error = InstallError::None;
        ESP_LOGI(kTag, "%s promoted: this is stable now", o_.running_ver);
        read_other(o_, other_);
        publish();
    }

    // From the card into the other slot. Never touches which slot boots.
    void install_sd() {
        o_.image_error = core::ota::ImageError::None;
        o_.install_pct = 0;
        if (o_.role != Role::Stable) return fail(InstallError::NotStable);
        if (!other_) return fail(InstallError::FlashFailed);   // no second slot
        if (!parked()) return fail(InstallError::Moving);
        if (!d_.card || !d_.card->present()) return fail(InstallError::NoCard);

        size_t size = 0;
        if (!hal::ok(d_.card->size_of(kUpdatePath, size))) return fail(InstallError::NoFile);

        o_.install = Install::Checking;
        o_.error = InstallError::None;
        publish();

        size_t got = 0;
        if (!hal::ok(d_.card->read_at(kUpdatePath, 0, g_chunk, core::ota::kHeaderBytes, got))) {
            return fail(InstallError::ReadFailed);
        }
        core::ota::ImageInfo info;
        o_.image_error = core::ota::check_header(g_chunk, got, size, other_->size, info);
        if (o_.image_error != core::ota::ImageError::None) {
            ESP_LOGW(kTag, "%s: %s", kUpdatePath, core::ota::image_error_name(o_.image_error));
            return fail(InstallError::BadImage);
        }
        ESP_LOGI(kTag, "installing %s (%u bytes) into %s", info.version,
                 static_cast<unsigned>(size), other_->label);

        // Erased a sector at a time as the writes reach it, so the cache
        // stalls are spread over the install rather than one 6 MB erase.
        esp_ota_handle_t h = 0;
        if (esp_ota_begin(other_, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
            return fail(InstallError::FlashFailed);
        }
        o_.install = Install::Writing;
        publish();

        size_t done = 0;
        while (done < size) {
            if (!parked()) {
                esp_ota_abort(h);
                read_other(o_, other_);
                return fail(InstallError::Moving);
            }
            const size_t want = size - done < kChunk ? size - done : kChunk;
            if (!hal::ok(d_.card->read_at(kUpdatePath, done, g_chunk, want, got)) ||
                got != want) {
                esp_ota_abort(h);
                read_other(o_, other_);
                return fail(InstallError::ReadFailed);
            }
            if (esp_ota_write(h, g_chunk, got) != ESP_OK) {
                esp_ota_abort(h);
                read_other(o_, other_);
                return fail(InstallError::FlashFailed);
            }
            done += got;
            const uint8_t pct = core::ota::percent(done, size);
            if (pct != o_.install_pct) {
                o_.install_pct = pct;
                publish();
            }
        }

        // The image's own checksum and hash, over everything just written.
        const esp_err_t end = esp_ota_end(h);
        read_other(o_, other_);
        if (end != ESP_OK) {
            return fail(end == ESP_ERR_OTA_VALIDATE_FAILED ? InstallError::VerifyFailed
                                                           : InstallError::FlashFailed);
        }
        o_.install = Install::Done;
        o_.error = InstallError::None;
        ESP_LOGI(kTag, "dev slot now holds %s", o_.other_ver);
        publish();
    }

    // Boot the other slot, once. The bootloader falls back to stable at the
    // next reset unless the rider promotes it in the meantime.
    void boot_dev() {
        if (o_.role != Role::Stable) return fail(InstallError::NotStable);
        if (!other_) return fail(InstallError::NoDev);
        if (!parked()) return fail(InstallError::Moving);
        read_other(o_, other_);
        if (o_.other != core::ota::Other::Ready) return fail(InstallError::NoDev);

        if (esp_ota_set_boot_partition(other_) != ESP_OK) {
            return fail(InstallError::FlashFailed);
        }
        ESP_LOGW(kTag, "booting once into %s, restarting", o_.other_ver);
        // Not the card's sync: the logger owns its open file, and calling
        // into it from here would race it. Up to its next sync (a few
        // seconds) of blackbox is the price of a restart the rider asked for.
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    Deps& d_;
    const esp_partition_t* running_ = nullptr;
    const esp_partition_t* other_ = nullptr;
    core::state::Ota o_;
};

void ota_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);
    static Ota ota(d);
    ota.boot();
    ota.run();
}

}  // namespace

void start_ota(Deps& d) {
    xTaskCreatePinnedToCore(ota_task, "ota", 4096, &d, 2, nullptr, 1);
}

}  // namespace services
