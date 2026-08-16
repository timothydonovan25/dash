// The SD card, in SPI mode, on the shared SPI2 bus.
//
// Not SDMMC: the card socket is wired to the same three SPI pins as the IMU
// and the expander, with its own chip select. That is slower than a 4-bit
// SDMMC bus and it is what the board has.
//
// The reason this sits behind an interface and gets drained by its own
// low-priority task: **an SD card can stall for hundreds of milliseconds**
// with no warning when its controller decides to do wear levelling or remap
// a block. `ctrl` has a 3 ms budget. Nothing on core 0 may ever wait on this.
#pragma once
#include "hal/i_storage.hpp"

namespace drivers {

class SdCard final : public hal::ICard {
public:
    hal::Status init() override;
    bool   present() const override { return mounted_; }
    hal::Status append(const char* path, const void* data, size_t len) override;
    hal::Status sync() override;

    void unmount();

    // Card-detect comes from the MCP23S17 (GPB7), not a GPIO, so the caller
    // passes what the expander last saw rather than this polling for it.
    void note_card_detect(bool inserted) { detected_ = inserted; }
    bool detected() const { return detected_; }

private:
    bool  mounted_ = false;
    bool  detected_ = false;
    void* card_ = nullptr;        // sdmmc_card_t*
    void* open_file_ = nullptr;   // FILE*, kept open between appends
    char  open_path_[64] = {};
    uint32_t unsynced_ = 0;
};

}  // namespace drivers
