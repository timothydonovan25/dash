// The SD card, in SPI mode, on the shared SPI2 bus.
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
    hal::Status append(const char* path, const void* data, size_t len) override;
    hal::Status sync() override;
    hal::Status size_of(const char* path, size_t& out) override;
    hal::Status read_at(const char* path, size_t offset, void* buf,
                        size_t len, size_t& got) override;

    // Closes the log file, which flushes it, and unmounts. Safe to call
    // with nothing mounted.
    void unmount();

    // Holds the lock for the whole walk. The logger's own file shows the
    // size it had at its last sync.
    hal::Status list(const char* dir, ListFn fn, void* ctx) override;
    hal::Status remove(const char* path) override;
    // Renames a file, closing it first if it is the one being appended to;
    // the next append() reopens it under whatever name it is given. Fails
    // rather than overwrite an existing `to`.
    hal::Status rename(const char* from, const char* to);
    bool exists(const char* path);

    // Reads the socket's card-detect switch. Configures the pin on first
    // use, so it works before init() and after unmount().
    bool card_detect();
    bool detected() const { return detected_; }

private:
    hal::Status sync_locked();

    StaticSemaphore_t lock_buf_;
    SemaphoreHandle_t lock_;
    bool  mounted_ = false;
    bool  detected_ = false;
    bool  cd_ready_ = false;
    void* card_ = nullptr;        // sdmmc_card_t*
    void* open_file_ = nullptr;   // FILE*, kept open between appends
    char  open_path_[64] = {};
    uint32_t unsynced_ = 0;
};

}  // namespace drivers
