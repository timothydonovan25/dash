#include "drivers/sdcard.hpp"
#include "sdcard_mount.hpp"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace drivers {
namespace {

constexpr const char* kMount = "/sd";

// Written every few seconds, so the cost of an fsync is amortised. Not every
// append: fsync on an SD card is where the multi-hundred-millisecond stalls
// live, and doing it per record would make the logger the slowest thing on
// the machine.
constexpr uint32_t kSyncEveryWrites = 64;

}  // namespace

namespace {
// Held for a whole filesystem call. An SD stall can take hundreds of
// milliseconds, and every holder is a core 1 task that can afford to wait
// for one -- but not forever: a card wedged inside a write must not take
// the other two tasks with it, so a caller that cannot get the lock in
// time gets Busy and tries again on its own schedule.
constexpr TickType_t kLockWait = pdMS_TO_TICKS(3000);
// unmount() waits up to this many kLockWaits.
constexpr int kUnmountLockTries = 5;

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

    void* card = nullptr;
    const hal::Status st = sdcard_mount(kMount, &card);
    if (!hal::ok(st)) return st;

    card_ = card;
    mounted_ = true;
    // The directories the firmware writes into. fopen() does not make a
    // missing one, so a fresh card would refuse every log and every track.
    // EEXIST on every mount after the first is the normal case.
    for (const char* dir : {"logs", "tracks", "games"}) {
        char full[32];
        std::snprintf(full, sizeof full, "%s/%s", kMount, dir);
        mkdir(full, 0777);
    }
    return hal::Status::Ok;
}

bool SdCard::unmount() {
    // Under the lock, waiting for it longer than the other calls do. It
    // used to go ahead without it after 3 s, and closed the file and the
    // volume under whichever task held it, mid-call. Every holder finishes
    // a call in well under this, a pulled card included (its I/O fails
    // fast), so waiting is safe; and if it ever does not come, unmounting
    // under a live call is the one thing worse than not unmounting.
    bool held = false;
    for (int i = 0; i < kUnmountLockTries && !held; ++i) {
        held = xSemaphoreTake(lock_, kLockWait) == pdTRUE;
    }
    if (!held) return false;
    struct Give {
        SemaphoreHandle_t l;
        ~Give() { xSemaphoreGive(l); }
    } give{lock_};
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
    return true;
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

hal::Status SdCard::remove(const char* path) {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    if (!path || !*path) return hal::Status::Unsupported;
    if (open_file_ && std::strncmp(open_path_, path, sizeof open_path_) == 0) {
        return hal::Status::Busy;
    }
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, path);
    return std::remove(full) == 0 ? hal::Status::Ok : hal::Status::NotPresent;
}

hal::Status SdCard::rename(const char* from, const char* to) {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    if (!from || !*from || !to || !*to) return hal::Status::Unsupported;
    if (open_file_ && std::strncmp(open_path_, from, sizeof open_path_) == 0) {
        std::fclose(static_cast<FILE*>(open_file_));
        open_file_ = nullptr;
        open_path_[0] = '\0';
        unsynced_ = 0;
    }
    char a[96], b[96];
    std::snprintf(a, sizeof a, "%s/%s", kMount, from);
    std::snprintf(b, sizeof b, "%s/%s", kMount, to);
    struct stat st;
    if (stat(b, &st) == 0) return hal::Status::Busy;      // never clobber
    return std::rename(a, b) == 0 ? hal::Status::Ok : hal::Status::IoError;
}

hal::Status SdCard::write_file(const char* path, const void* data, size_t len) {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    if (!path || !*path || (!data && len)) return hal::Status::Unsupported;
    // Never the file append() is holding open: that is the log.
    if (open_file_ && std::strncmp(open_path_, path, sizeof open_path_) == 0) {
        return hal::Status::Busy;
    }
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, path);
    FILE* f = std::fopen(full, "wb");
    if (!f) return hal::Status::IoError;
    const size_t written = len ? std::fwrite(data, 1, len, f) : 0;
    // fsync before the close, so a rename after this lands on a file whose
    // bytes are on the card and not in the FAT layer's buffer.
    const bool flushed = std::fflush(f) == 0 && fsync(fileno(f)) == 0;
    const bool closed = std::fclose(f) == 0;
    return written == len && flushed && closed ? hal::Status::Ok : hal::Status::IoError;
}

hal::Status SdCard::write_at(const char* path, size_t offset, const void* data,
                             size_t len) {
    const Hold hold(lock_);
    if (!hold.held()) return hal::Status::Busy;
    if (!mounted_) return hal::Status::NotPresent;
    if (!path || !*path || (!data && len)) return hal::Status::Unsupported;
    if (open_file_ && std::strncmp(open_path_, path, sizeof open_path_) == 0) {
        return hal::Status::Busy;
    }
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, path);
    // Only ever on the end: a piece that does not follow the last is an
    // upload that lost one, and is refused rather than written with a hole.
    if (offset != 0) {
        struct stat st;
        if (stat(full, &st) != 0 || static_cast<size_t>(st.st_size) != offset) {
            return hal::Status::Unsupported;
        }
    }
    FILE* f = std::fopen(full, offset == 0 ? "wb" : "ab");
    if (!f) return hal::Status::IoError;
    const size_t written = len ? std::fwrite(data, 1, len, f) : 0;
    const bool closed = std::fclose(f) == 0;
    return written == len && closed ? hal::Status::Ok : hal::Status::IoError;
}

bool SdCard::exists(const char* path) {
    const Hold hold(lock_);
    if (!hold.held() || !mounted_ || !path) return false;
    char full[96];
    std::snprintf(full, sizeof full, "%s/%s", kMount, path);
    struct stat st;
    return stat(full, &st) == 0;
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
