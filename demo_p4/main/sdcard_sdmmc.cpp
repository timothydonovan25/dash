// The demo board's half of drivers::SdCard (components/drivers/src/
// sdcard_mount.hpp): the Guition's microSD slot, 4-bit SDMMC on slot 0.
// Everything else — the lock, the file calls, the directories — is the
// bike's own sdcard.cpp, compiled in by source.
//
// The P4 has one SDMMC controller with two slots, and ESP-Hosted already
// owns it: its constructor runs sdmmc_host_init() for the C6's SDIO link on
// slot 1 before app_main. A second sdmmc_host_init() fails ("no available sd
// host controller"), and one made first would make ESP-Hosted's fail and
// take BLE and Wi-Fi with it. So the card's host init is a no-op that only
// adds slot 0 to the controller ESP-Hosted made; if it has not made one,
// sdmmc_host_init_slot() says so (ESP_ERR_INVALID_STATE) and the card is
// absent. An unmount removes slot 0 only (SDMMC_HOST_FLAG_DEINIT_ARG).
#include "drivers/sdcard.hpp"
#include "sdcard_mount.hpp"
#include "p4_pins.hpp"

#include "driver/sdmmc_host.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace drivers {
namespace {

constexpr const char* kTag = "sdmmc";

esp_err_t controller_already_up() { return ESP_OK; }

}  // namespace

// The slot has no card-detect switch (J1 pins 9 and 10 are the shell), so
// a card is "there" until a mount says otherwise.
bool SdCard::card_detect() { return detected_ = true; }

hal::Status sdcard_mount(const char* mount_point, void** card_out) {
    // TF_VCC. Acquired once and never released: the card is powered for
    // as long as the board is, as it would be with a fixed regulator.
    static esp_ldo_channel_handle_t ldo = nullptr;
    if (!ldo) {
        esp_ldo_channel_config_t cfg = {};
        cfg.chan_id = p4pins::kSdLdoChannel;
        cfg.voltage_mv = p4pins::kSdLdoMv;
        const esp_err_t err = esp_ldo_acquire_channel(&cfg, &ldo);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "LDO VO4 for the card: %s", esp_err_to_name(err));
            return hal::Status::IoError;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.init = &controller_already_up;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   // 20 MHz x 4 bits: a WAD in under a second

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = static_cast<gpio_num_t>(p4pins::kSdClk);
    slot.cmd = static_cast<gpio_num_t>(p4pins::kSdCmd);
    slot.d0 = static_cast<gpio_num_t>(p4pins::kSdD0);
    slot.d1 = static_cast<gpio_num_t>(p4pins::kSdD1);
    slot.d2 = static_cast<gpio_num_t>(p4pins::kSdD2);
    slot.d3 = static_cast<gpio_num_t>(p4pins::kSdD3);
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    esp_vfs_fat_mount_config_t mount = {};
    // Never format: whatever is on the card stays there. A 32 GB card is
    // SDHC and comes as FAT32; this FATFS has no exFAT, so a card that was
    // reformatted exFAT does not mount and says so here.
    mount.format_if_mount_failed = false;
    mount.max_files = 6;   // Doom holds its WAD open beside the page's files
    mount.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "mount: %s", esp_err_to_name(err));
        return err == ESP_ERR_TIMEOUT ? hal::Status::Timeout : hal::Status::NotPresent;
    }
    sdmmc_card_print_info(stdout, card);
    *card_out = card;
    return hal::Status::Ok;
}

}  // namespace drivers
