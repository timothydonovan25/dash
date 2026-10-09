#include "drivers/sdcard.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace drivers {
namespace {

constexpr const char* kMount = "/sd";

// Written every few seconds, so the cost of an fsync is amortised. Not every
// append: fsync on an SD card is where the multi-hundred-millisecond stalls
// live, and doing it per record would make the logger the slowest thing on
// the machine.
constexpr uint32_t kSyncEveryWrites = 64;

hal::Status from_esp(esp_err_t err) {
    switch (err) {
        case ESP_OK: return hal::Status::Ok;
        case ESP_ERR_TIMEOUT: return hal::Status::Timeout;
        case ESP_ERR_NOT_FOUND: return hal::Status::NotPresent;
        case ESP_ERR_INVALID_STATE: return hal::Status::Busy;
        default: return hal::Status::IoError;
    }
}

}  // namespace

bool SdCard::card_detect() {
    if (!cd_ready_) {
        // R16 is the pull-up; the internal one covers it being unpopulated.
        gpio_config_t cd = {};
        cd.pin_bit_mask = 1ull << bsp::kSdCardDetect;
        cd.mode = GPIO_MODE_INPUT;
        cd.pull_up_en = GPIO_PULLUP_ENABLE;
        cd.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cd.intr_type = GPIO_INTR_DISABLE;
        cd_ready_ = gpio_config(&cd) == ESP_OK;
        // A pin that would not configure reads as "inserted", so a broken
        // detect line degrades to "try to mount" rather than "never log".
        if (!cd_ready_) return detected_ = true;
    }
    const int level = gpio_get_level(static_cast<gpio_num_t>(bsp::kSdCardDetect));
    detected_ = bsp::kSdCardDetectActiveLow ? level == 0 : level != 0;
    return detected_;
}

namespace {
// Held for a whole filesystem call. An SD stall can take hundreds of
// milliseconds, and every holder is a core 1 task that can afford to wait
// for one -- but not forever: a card wedged inside a write must not take
// the other two tasks with it, so a caller that cannot get the lock in
// time gets Busy and tries again on its own schedule.
constexpr TickType_t kLockWait = pdMS_TO_TICKS(3000);

class Hold {
public:
    explicit Hold(SemaphoreHandle_t l)
        : l_(l), held_(xSemaphoreTake(l_, kLockWait) == pdTRUE) {}
    ~Hold() { if (held_) xSemaphoreGive(l_); }
    bool held() const { return held_; }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
private:
    SemaphoreHandle_t l_;
    bool held_;
};
}  // namespace

hal::Status SdCard::init() {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (mounted_) return hal::Status::Ok;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = bsp::kSpiHzSd / 1000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = static_cast<gpio_num_t>(bsp::kCsSd);
    slot.host_id = SPI2_HOST;
    // Card detect is read by card_detect() and acted on by the storage task,
    // which owns the unmount. Handing the pin to sdspi as well would give it
    // two readers with two ideas of what a pulled card means.
    slot.gpio_cd = GPIO_NUM_NC;
    slot.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_mount_config_t mount = {};
    // Never format. A card that will not mount is a card to look at on a
    // laptop, not one to erase — it may be the only copy of a session.
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    mount.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    const esp_err_t err =
        esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mount, &card);
    if (err != ESP_OK) return from_esp(err);

    card_ = card;
    mounted_ = true;
    return hal::Status::Ok;
}

void SdCard::unmount() {
    // Unmounting without the lock is still better than not unmounting: by
    // then the card is gone or the rider has been told it is safe to pull.
    const Hold hold(lock_);
    if (open_file_) {
        std::fclose(static_cast<FILE*>(open_file_));
        open_file_ = nullptr;
        open_path_[0] = '\0';
    }
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount(kMount, static_cast<sdmmc_card_t*>(card_));
        mounted_ = false;
        card_ = nullptr;
    }
}

hal::Status SdCard::append(const char* path, const void* data, size_t len) {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    if (!path || !data || len == 0) return hal::Status::Unsupported;

    // The file is held open between calls. Opening per append costs a
    // directory walk and a FAT update every time, which on this bus is
    // milliseconds the logger does not have to spend.
    if (!open_file_ || std::strncmp(open_path_, path, sizeof open_path_) != 0) {
        if (open_file_) std::fclose(static_cast<FILE*>(open_file_));
        char full[96];
        std::snprintf(full, sizeof full, "%s/%s", kMount, path);
        open_file_ = std::fopen(full, "ab");
        if (!open_file_) {
            open_path_[0] = '\0';
            return hal::Status::IoError;
        }
        std::snprintf(open_path_, sizeof open_path_, "%s", path);
    }

    const size_t written =
        std::fwrite(data, 1, len, static_cast<FILE*>(open_file_));
    if (written != len) return hal::Status::IoError;

    if (++unsynced_ >= kSyncEveryWrites) return sync_locked();
    return hal::Status::Ok;
}

hal::Status SdCard::list(const char* dir, ListFn fn, void* ctx) {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, dir);
    DIR* d = opendir(full);
    if (!d) return hal::Status::NotPresent;
    while (const dirent* e = readdir(d)) {
        if (e->d_type != DT_REG) continue;
        char path[96 + sizeof e->d_name];
        std::snprintf(path, sizeof path, "%s/%s", full, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        fn(e->d_name, static_cast<size_t>(st.st_size), ctx);
    }
    closedir(d);
    return hal::Status::Ok;
}

hal::Status SdCard::size_of(const char* path, size_t& out) {
    out = 0;
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, path);
    struct stat st;
    if (stat(full, &st) != 0) return hal::Status::NotPresent;
    out = static_cast<size_t>(st.st_size);
    return hal::Status::Ok;
}

hal::Status SdCard::read_at(const char* path, size_t offset, void* buf,
                            size_t len, size_t& got) {
    got = 0;
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    if (!path || !buf) return hal::Status::Unsupported;
    // Its own FILE*, never the logger's: the two run on different tasks,
    // and FATFS in IDF is reentrant but a shared FILE* is not.
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, path);
    FILE* f = std::fopen(full, "rb");
    if (!f) return hal::Status::NotPresent;
    hal::Status s = hal::Status::Ok;
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        s = hal::Status::IoError;
    } else {
        got = std::fread(buf, 1, len, f);
        if (got < len && std::ferror(f)) s = hal::Status::IoError;
    }
    std::fclose(f);
    return s;
}

hal::Status SdCard::sync() {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    return sync_locked();
}

hal::Status SdCard::sync_locked() {
    if (!open_file_) return hal::Status::Ok;
    unsynced_ = 0;
    // fflush pushes our buffer at the filesystem; fsync makes FAT commit it.
    // Both are needed for a log that survives the ignition being switched off
    // mid-ride, which is the normal way this bike stops.
    if (std::fflush(static_cast<FILE*>(open_file_)) != 0) return hal::Status::IoError;
    if (fsync(fileno(static_cast<FILE*>(open_file_))) != 0) return hal::Status::IoError;
    return hal::Status::Ok;
}

}  // namespace drivers
