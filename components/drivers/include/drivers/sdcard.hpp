// The SD card, in SPI mode, on the shared SPI2 bus. (The mount is the one
// board-specific part, src/sdcard_spi.cpp; the Guition demo links its own,
// over SDMMC — src/sdcard_mount.hpp.)
//
// Not SDMMC: the card socket is wired to the same three SPI pins as the IMU,
// with its own chip select. That is slower than a 4-bit SDMMC bus and it is
// what the board has. On the P4 it also keeps the card off the SDMMC
// controller, which the module uses for its link to the C6.
//
// The reason this sits behind an interface and gets drained by its own
// low-priority task: **an SD card can stall for hundreds of milliseconds**
// with no warning when its controller decides to do wear levelling or remap
// a block. `ctrl` has a 3 ms budget. Nothing on core 0 may ever wait on this.
#pragma once
#include "hal/i_storage.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace drivers {

// Three tasks touch the card -- `logger` writes, `ota` reads, `storage`
// mounts and unmounts -- so every call that reaches the filesystem holds one
// lock. Without it an unmount (a pulled card, or CONFIG's Eject) could land
// in the middle of a log write and leave the FAT half-updated.
class SdCard final : public hal::ICard {
public:
    SdCard() : lock_(xSemaphoreCreateMutexStatic(&lock_buf_)) {}
    SdCard(const SdCard&) = delete;
    SdCard& operator=(const SdCard&) = delete;

    hal::Status init() override;
    bool   present() const override { return mounted_; }
    // Files appended to are held open, up to kOpenFiles of them (the
    // blackbox and the five detail logs), the least recently written one
    // closed to make room for another. sync() commits every one of them.
    hal::Status append(const char* path, const void* data, size_t len) override;
    hal::Status sync() override;
    hal::Status size_of(const char* path, size_t& out) override;
    hal::Status read_at(const char* path, size_t offset, void* buf,
                        size_t len, size_t& got) override;

    // Closes the log files, which flushes them, and unmounts. Safe to call
    // with nothing mounted. False if the card lock never came free, in
    // which case nothing was touched and the caller tries again.
    bool unmount();

    // Holds the lock for the whole walk. The logger's own file shows the
    // size it had at its last sync.
    hal::Status list(const char* dir, ListFn fn, void* ctx, bool sizes = true) override;
    hal::Status remove(const char* path) override;
    // Renames a file, closing it first if it is one being appended to;
    // the next append() reopens it under whatever name it is given. Fails
    // rather than overwrite an existing `to`.
    hal::Status rename(const char* from, const char* to) override;
    hal::Status write_file(const char* path, const void* data, size_t len) override;
    hal::Status write_at(const char* path, size_t offset, const void* data,
                         size_t len) override;
    bool exists(const char* path);

    // Reads the socket's card-detect switch. Configures the pin on first
    // use, so it works before init() and after unmount().
    bool card_detect();
    bool detected() const { return detected_; }

    static constexpr int kOpenFiles = 6;

private:
    struct Open {
        void*    file = nullptr;     // FILE*
        char     path[64] = {};
        uint32_t unsynced = 0;
        uint32_t used = 0;           // append() counter when last written
    };
    Open* find_open(const char* path);
    void  close_open(Open& o);
    void  close_all();

    // read_at()'s files, kept open between calls (the map and the search
    // index: a route reads hundreds of 4 kB blocks, and an fopen each was
    // 30 ms of FAT directory walk). Used only under the lock, so a FILE* is
    // never in two tasks at once. Closed whenever that file is removed,
    // renamed or written, and at unmount.
    struct Reader {
        void*    file = nullptr;     // FILE*
        char     path[64] = {};
        uint32_t used = 0;
    };
    static constexpr int kReaders = 2;
    Reader*  reader(const char* path);
    void     close_reader(const char* path);
    Reader   readers_[kReaders];
    uint32_t reads_ = 0;
    hal::Status sync_one(Open& o);

    StaticSemaphore_t lock_buf_;
    SemaphoreHandle_t lock_;
    bool  mounted_ = false;
    bool  detected_ = false;
    bool  cd_ready_ = false;
    void* card_ = nullptr;        // sdmmc_card_t*
    Open     open_[kOpenFiles];   // kept open between appends
    uint32_t appends_ = 0;
};

}  // namespace drivers
