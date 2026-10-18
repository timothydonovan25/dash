// The one board-specific half of drivers::SdCard: how the card is reached.
// sdcard.cpp is the filesystem calls and the lock, the same on every board;
// each board links one file that defines these two (and SdCard::card_detect):
//   sdcard_spi.cpp                 the bike, SPI mode on SPI2 (bsp/pins.hpp)
//   demo_p4/main/sdcard_sdmmc.cpp  the Guition demo, 4-bit SDMMC
#pragma once
#include "hal/status.hpp"

namespace drivers {

// Mounts the card's FAT volume at `mount_point`, never formatting it, and
// hands back the sdmmc_card_t* for the unmount. Called under the card lock.
hal::Status sdcard_mount(const char* mount_point, void** card_out);

}  // namespace drivers
