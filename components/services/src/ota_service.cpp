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
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
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
// PSRAM, to keep internal .bss small (CLAUDE.md, internal RAM ceiling):
// flash writes and card reads from here go through IDF's own internal
// bounce buffers, which only an install pays for.
EXT_RAM_BSS_ATTR uint8_t g_chunk[kChunk];

// An upload that stops sending for this long has been abandoned: the phone
// left the hotspot, or the page was closed.
constexpr TickType_t kStreamStall = pdMS_TO_TICKS(10000);

// Where an install's bytes come from. Read in order, once each: an upload
// cannot seek, so the card is read the same way.
class Source {
public:
    virtual ~Source() = default;
    // Fills `buf` with exactly `len` bytes, or says why not.
    virtual bool read(void* buf, size_t len) = 0;
};

class CardSource final : public Source {
public:
    CardSource(hal::ICard& card, const char* path) : card_(card), path_(path) {}
    bool read(void* buf, size_t len) override {
        size_t got = 0;
        if (!hal::ok(card_.read_at(path_, at_, buf, len, got)) || got != len) return false;
        at_ += got;
        return true;
    }
private:
    hal::ICard& card_;
    const char* path_;
    size_t at_ = 0;
};

class StreamSource final : public Source {
public:
    explicit StreamSource(StreamBufferHandle_t sb) : sb_(sb) {}
    bool read(void* buf, size_t len) override {
        auto* p = static_cast<uint8_t*>(buf);
        size_t got = 0;
        while (got < len) {
            const size_t n = xStreamBufferReceive(sb_, p + got, len - got, kStreamStall);
            if (n == 0) return false;          // stalled
            got += n;
        }
        return true;
    }
private:
    StreamBufferHandle_t sb_;
};

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

// Not strncpy: at -O2 GCC proves it can truncate and -Werror stops the
// build (stringop-truncation), though truncating is the point here.
void copy_str(char* dst, size_t cap, const char* src) {
    const size_t n = strnlen(src, cap - 1);
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

// What the other slot holds, refreshed after anything that changes it.
void read_other(core::state::Ota& o, const esp_partition_t* other) {
    esp_app_desc_t desc;
    const bool has = other && esp_ota_get_partition_description(other, &desc) == ESP_OK;
    o.other = core::ota::other_from(has, state_of(other));
    if (has) {
        copy_str(o.other_ver, sizeof o.other_ver, desc.version);
        core::ota::iso_date(desc.date, o.other_date, sizeof o.other_date);
    } else {
        o.other_ver[0] = '\0';
        o.other_date[0] = '\0';
    }
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
        core::ota::iso_date(esp_app_get_description()->date, o_.running_date,
                            sizeof o_.running_date);
        copy_str(o_.running_slot, sizeof o_.running_slot, running_ ? running_->label : "?");
        read_other(o_, other_);
        ESP_LOGI(kTag, "running %s from %s, %s; other slot %s",
                 o_.running_ver, o_.running_slot, core::ota::role_name(o_.role),
                 o_.other == core::ota::Other::Ready ? o_.other_ver : "empty");
        publish();
    }

    void run() {
        uint32_t request_gen = d_.slots->ota_request.generation();
        uint32_t upload_gen = d_.slots->ota_upload.generation();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(kIdleWaitMs));

            const uint32_t ugen = d_.slots->ota_upload.generation();
            if (ugen != upload_gen) {
                upload_gen = ugen;
                install_upload(d_.slots->ota_upload.get().size);
            }

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
        if (!may_install()) return;
        if (!d_.card || !d_.card->present()) return fail(InstallError::NoCard);

        size_t size = 0;
        if (!hal::ok(d_.card->size_of(kUpdatePath, size))) return fail(InstallError::NoFile);
        CardSource src(*d_.card, kUpdatePath);
        install(src, size, kUpdatePath);
    }

    // From the web page, through Deps::ota_stream. The page's request is
    // waiting on the other end of the stream, so a refusal still has to
    // drain what it sends -- otherwise it would block until it timed out.
    void install_upload(uint32_t size) {
        auto* sb = static_cast<StreamBufferHandle_t>(d_.ota_stream);
        o_.image_error = core::ota::ImageError::None;
        o_.install_pct = 0;
        if (!sb) return fail(InstallError::ReadFailed);
        if (!may_install()) return drain(sb);
        StreamSource src(sb);
        if (!install(src, size, "upload")) drain(sb);
    }

    // Whether the dev slot may be written right now; says why not if not.
    bool may_install() {
        if (o_.role != Role::Stable) { fail(InstallError::NotStable); return false; }
        if (!other_) { fail(InstallError::FlashFailed); return false; }   // no second slot
        if (!parked()) { fail(InstallError::Moving); return false; }
        return true;
    }

    // Discards whatever is still arriving, until the sender stops.
    static void drain(StreamBufferHandle_t sb) {
        while (xStreamBufferReceive(sb, g_chunk, kChunk, pdMS_TO_TICKS(1000)) > 0) {}
    }

    // `size` bytes from `src` into the other slot. True if they all arrived
    // and passed; false with the status published if not.
    bool install(Source& src, size_t size, const char* what) {
        o_.install = Install::Checking;
        o_.error = InstallError::None;
        publish();

        // The header first, then the same bytes are the start of the image:
        // the source is read once, so nothing is read twice.
        const size_t head = size < core::ota::kHeaderBytes ? size : core::ota::kHeaderBytes;
        if (!src.read(g_chunk, head)) { fail(InstallError::ReadFailed); return false; }
        core::ota::ImageInfo info;
        o_.image_error = core::ota::check_header(g_chunk, head, size, other_->size, info);
        if (o_.image_error == core::ota::ImageError::None) {
            o_.image_error = core::ota::check_board(o_.running_ver, info.version);
        }
        if (o_.image_error != core::ota::ImageError::None) {
            ESP_LOGW(kTag, "%s (%s): %s", what, info.version,
                     core::ota::image_error_name(o_.image_error));
            fail(InstallError::BadImage);
            return false;
        }
        ESP_LOGI(kTag, "installing %s (%u bytes, %s) into %s", info.version,
                 static_cast<unsigned>(size), what, other_->label);

        // Erased a sector at a time as the writes reach it, so the cache
        // stalls are spread over the install rather than one 6 MB erase.
        esp_ota_handle_t h = 0;
        if (esp_ota_begin(other_, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
            fail(InstallError::FlashFailed);
            return false;
        }
        o_.install = Install::Writing;
        publish();

        auto abort_with = [&](InstallError e) {
            esp_ota_abort(h);
            read_other(o_, other_);
            fail(e);
            return false;
        };

        size_t got = head;
        size_t done = 0;
        for (;;) {
            if (esp_ota_write(h, g_chunk, got) != ESP_OK) {
                return abort_with(InstallError::FlashFailed);
            }
            done += got;
            const uint8_t pct = core::ota::percent(done, size);
            if (pct != o_.install_pct) {
                o_.install_pct = pct;
                publish();
            }
            if (done >= size) break;
            // A full stream never blocks the read below, and with `render`
            // taking the rest of core 1 its idle task starved: the task
            // watchdog panicked the board 75 % into a phone upload. One tick
            // a chunk, ~0.6 s over a 2.4 MB image.
            vTaskDelay(1);
            if (!parked()) return abort_with(InstallError::Moving);
            got = size - done < kChunk ? size - done : kChunk;
            if (!src.read(g_chunk, got)) return abort_with(InstallError::ReadFailed);
        }

        // The image's own checksum and hash, over everything just written.
        const esp_err_t end = esp_ota_end(h);
        read_other(o_, other_);
        if (end != ESP_OK) {
            fail(end == ESP_ERR_OTA_VALIDATE_FAILED ? InstallError::VerifyFailed
                                                    : InstallError::FlashFailed);
            return false;
        }
        o_.install = Install::Done;
        o_.error = InstallError::None;
        ESP_LOGI(kTag, "dev slot now holds %s", o_.other_ver);
        publish();
        return true;
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
    // 16 kB: four upload chunks in flight while the flash writes catch up.
    // PSRAM: internal RAM is what the boot needs (16 kB more here and the
    // demo could not create its idle tasks), and the bytes are copied out to
    // g_chunk (PSRAM too; IDF bounces it) before any flash write.
    static StaticStreamBuffer_t sb_ctl;
    EXT_RAM_BSS_ATTR static uint8_t sb_mem[16 * 1024 + 1];
    if (!d.ota_stream) {
        d.ota_stream = xStreamBufferCreateStatic(sizeof sb_mem - 1, 1, sb_mem, &sb_ctl);
    }
    xTaskCreatePinnedToCore(ota_task, "ota", 4096, &d, 2, nullptr, 1);
}

}  // namespace services
