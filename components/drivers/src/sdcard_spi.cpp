// The bike's card: SPI mode on the shared SPI2 bus, with its own chip select
// and a card-detect switch. See sdcard_mount.hpp.
#include "drivers/sdcard.hpp"
#include "sdcard_mount.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace drivers {
namespace {

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

hal::Status sdcard_mount(const char* mount_point, void** card_out) {
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
        esp_vfs_fat_sdspi_mount(mount_point, &host, &slot, &mount, &card);
    if (err != ESP_OK) return from_esp(err);
    *card_out = card;
    return hal::Status::Ok;
}

}  // namespace drivers
