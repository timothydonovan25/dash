// `ota` — core 1, priority 2, event driven (§11.7).
//
// Stable and dev firmware in the two app slots, and updating the dev one.
// The decisions are core::ota's, host-tested; this task does the flash
// calls and is the one writer of the `ota` slot.
//
// At boot it works out what the running image is, from the bootloader's own
// state rather than from anything stored (see core/ota/ota.hpp):
//   - stable: carries on; on a power-on it lets the UI offer the chooser;
//   - dev: carries on and never confirms, so the next reset is stable again;
//   - confirming: a promoted image, which confirms itself once it has run
//     the dash for core::ota::kConfirmAfterMs. A crash before that and the
//     bootloader puts the old stable back.
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

// The promote marker: the slot index a promotion is waiting on, or -1. In
// its own namespace, so nothing else's keys can collide with it. A float
// because that is what IStorage stores; small integers are exact.
constexpr const char* kNsOta = "ota";
constexpr uint16_t    kKeyPromote = 0x0001;

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

int slot_index(const esp_partition_t* p) {
    return p ? static_cast<int>(p->subtype) - ESP_PARTITION_SUBTYPE_APP_OTA_MIN : -1;
}

int read_marker(Deps& d) {
    float v = -1.0f;
    if (!d.storage || !hal::ok(d.storage->read_f32(kNsOta, kKeyPromote, v))) return -1;
    return v < 0.0f ? -1 : static_cast<int>(v + 0.5f);
}

bool write_marker(Deps& d, int slot) {
    if (!d.storage) return false;
    return hal::ok(d.storage->write_f32(kNsOta, kKeyPromote, static_cast<float>(slot))) &&
           hal::ok(d.storage->commit());
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

// A restart the dash asked for itself is not a power-on, so a boot into dev
// does not open the chooser again on its way back.
bool powered_on() {
    const esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT;
}

class Ota {
public:
    explicit Ota(Deps& d) : d_(d) {}

    void boot() {
        running_ = esp_ota_get_running_partition();
        other_ = esp_ota_get_next_update_partition(running_);
        const int marker = read_marker(d_);
        const core::ota::Boot b =
            core::ota::role_of(state_of(running_), slot_index(running_), marker);

        o_.known = true;
        o_.role = b.role;
        o_.promotion_failed = b.promotion_failed;
        copy_str(o_.running_ver, sizeof o_.running_ver, esp_app_get_description()->version);
        copy_str(o_.running_slot, sizeof o_.running_slot, running_ ? running_->label : "?");
        read_other(o_, other_);
        o_.boot_window = b.role == Role::Stable && powered_on();

        if (b.clear_marker) write_marker(d_, -1);
        if (b.promotion_failed) {
            ESP_LOGW(kTag, "the promoted image never confirmed; still on the old stable");
        }
        ESP_LOGI(kTag, "running %s from %s, %s; other slot %s",
                 o_.running_ver, o_.running_slot, core::ota::role_name(o_.role),
                 o_.other == core::ota::Other::Ready ? o_.other_ver : "empty");
        publish();
    }

    void run() {
        uint32_t request_gen = d_.slots->ota_request.generation();
        uint32_t motion_gen = d_.slots->motion.generation();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));

            if (o_.role == Role::Confirming) {
                // `ctrl` publishes motion at 500 Hz; a generation that moved
                // in the last 100 ms is a ctrl still running.
                const uint32_t g = d_.slots->motion.generation();
                const bool ctrl_alive = g != motion_gen;
                motion_gen = g;
                const uint16_t hz = d_.system ? d_.system->render_hz_x10 : 0;
                if (core::ota::should_confirm(d_.clock->now_ms(), hz, ctrl_alive)) confirm();
            }

            const uint32_t gen = d_.slots->ota_request.generation();
            if (gen == request_gen) continue;
            request_gen = gen;
            switch (d_.slots->ota_request.get().op) {
                case core::state::OtaRequest::InstallSd:   install_sd(); break;
                case core::state::OtaRequest::BootDevOnce: boot_dev(false); break;
                case core::state::OtaRequest::Promote:     boot_dev(true); break;
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

    void confirm() {
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
            ESP_LOGE(kTag, "could not confirm; the next reset goes back to the old stable");
            return;
        }
        write_marker(d_, -1);
        o_.role = Role::Stable;
        ESP_LOGI(kTag, "promoted image confirmed: this is stable now");
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

    // Boot the other slot. Once, or as a promotion on probation. Either way
    // it is the bootloader that falls back if the new image does not make it.
    void boot_dev(bool promote) {
        if (o_.role != Role::Stable) return fail(InstallError::NotStable);
        if (!other_) return fail(InstallError::NoDev);
        if (!parked()) return fail(InstallError::Moving);
        read_other(o_, other_);
        if (o_.other != core::ota::Other::Ready) return fail(InstallError::NoDev);

        if (promote && !write_marker(d_, slot_index(other_))) {
            return fail(InstallError::FlashFailed);
        }
        if (esp_ota_set_boot_partition(other_) != ESP_OK) {
            if (promote) write_marker(d_, -1);
            return fail(InstallError::FlashFailed);
        }
        ESP_LOGW(kTag, "%s %s, restarting", promote ? "promoting" : "booting once into",
                 o_.other_ver);
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
