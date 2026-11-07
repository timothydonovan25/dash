#include "drivers/usb_drive.hpp"

#include "bsp/pins.hpp"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/usb_wrap_struct.h"
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "tusb.h"
#include <atomic>

namespace drivers {
namespace {

constexpr const char* kTag = "usb_drive";

// The card for the laptop: its own descriptor, the SPI device the dash's
// mount had, no filesystem on top.
sdmmc_card_t g_card;

// The laptop ejected the drive (SCSI START STOP UNIT, "eject"): esp_tinyusb
// hands the storage back to the app and says so here. Set on TinyUSB's task,
// read on `storage`'s.
std::atomic<bool> g_ejected{false};

void on_msc_event(tinyusb_msc_storage_handle_t, tinyusb_msc_event_t* e, void*) {
    if (!e) return;
    // The storage gone back to the app: the laptop ejected it (or went).
    if ((e->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE && e->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) ||
        e->id == TINYUSB_MSC_EVENT_MOUNT_FAILED)
        g_ejected.store(true);
}

// Which peripheral the USB-C's PHY (PHY 0) belongs to.
void phy_to_otg(bool otg) {
    if (otg) {
        // USB-Serial/JTAG's pads off first: it is about to sit on PHY 1,
        // whose pads are GPIO26/27, the panel's D6/D7.
        USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
        LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;     // software chooses
        LP_SYS.usb_ctrl.sw_usb_phy_sel = 1;        // OTG on PHY 0 (the USB-C), USJ on PHY 1
    } else {
        // OTG's pads off before it goes back to PHY 1. TinyUSB's PHY setup
        // turned them on (usb_wrap_ll_phy_set_defaults) and nothing turns
        // them off; left on, OTG back on PHY 1 makes GPIO26/27 USB pads, and
        // the panel's D6/D7 garbled the screen after a drive session until
        // the next reset (seen on the bike, 2026-11-05).
        USB_WRAP.otg_conf.usb_pad_enable = 0;
        LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
        LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;     // back to the hardware default: USJ on PHY 0
        USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;  // the laptop sees the serial port again
    }
}

}  // namespace

void UsbDrive::restore_phy() {
    // LP registers live through a software reset or a panic: a crash in drive
    // mode would otherwise come back with no console on the USB-C.
    if (LP_SYS.usb_ctrl.sw_hw_usb_phy_sel || !USB_SERIAL_JTAG.conf0.usb_pad_enable ||
        USB_WRAP.otg_conf.usb_pad_enable)
        phy_to_otg(false);
}

hal::Status UsbDrive::start() {
    if (active_) return hal::Status::Ok;
    // The card on its own: the same bus and chip select as the dash's mount.
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = bsp::kSpiHzSd / 1000;
    esp_err_t err = sdspi_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return hal::Status::IoError;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = static_cast<gpio_num_t>(bsp::kCsSd);
    slot.host_id = SPI2_HOST;
    slot.gpio_cd = GPIO_NUM_NC;
    slot.gpio_wp = GPIO_NUM_NC;
    sdspi_dev_handle_t dev = -1;
    if (sdspi_host_init_device(&slot, &dev) != ESP_OK) {
        sdspi_host_deinit();
        return hal::Status::IoError;
    }
    dev_ = dev;
    host.slot = dev;
    if (sdmmc_card_init(&host, &g_card) != ESP_OK) {
        ESP_LOGW(kTag, "card did not answer");
        stop();
        return hal::Status::NotPresent;
    }

    phy_to_otg(true);
    tinyusb_config_t cfg = {};
    cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    cfg.phy.skip_setup = false;
    cfg.phy.self_powered = false;
    cfg.phy.vbus_monitor_io = -1;
    cfg.task.size = 4096;
    cfg.task.priority = 5;
    cfg.task.xCoreID = 1;                   // core 0 is the real-time island
    if (tinyusb_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "TinyUSB would not start");
        stop();
        return hal::Status::IoError;
    }
    usb_ = true;
    g_ejected.store(false);
    tinyusb_msc_driver_config_t drv = {};
    // Auto mount on: with it off, esp_tinyusb ignores the laptop's eject
    // and says nothing (seen on the bike). On, an eject or an unplug mounts
    // the card to the app for a moment and says so (on_msc_event); stop()
    // then drops that mount and the dash remounts the card its own way.
    drv.user_flags.auto_mount_off = 0;
    drv.callback = on_msc_event;
    tinyusb_msc_install_driver(&drv);       // already installed is fine
    tinyusb_msc_storage_config_t sc = {};
    sc.medium.card = &g_card;
    sc.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    tinyusb_msc_storage_handle_t h = nullptr;
    if (tinyusb_msc_new_storage_sdmmc(&sc, &h) != ESP_OK) {
        ESP_LOGW(kTag, "MSC storage would not start");
        stop();
        return hal::Status::IoError;
    }
    storage_ = h;
    active_ = true;
    return hal::Status::Ok;
}

void UsbDrive::stop() {
    if (storage_) tinyusb_msc_delete_storage(static_cast<tinyusb_msc_storage_handle_t>(storage_));
    storage_ = nullptr;
    if (usb_) {
        tinyusb_msc_uninstall_driver();
        tinyusb_driver_uninstall();
        usb_ = false;
    }
    phy_to_otg(false);
    if (dev_ >= 0) sdspi_host_remove_device(dev_);
    dev_ = -1;
    sdspi_host_deinit();
    active_ = false;
}

// Let go: ejected (the clean way, "Eject" in a file manager or `eject`),
// unplugged, or the port powered off (udisksctl power-off): the bus then
// suspends and the device stays "mounted", so suspended counts as gone.
bool UsbDrive::host_connected() const {
    return active_ && tud_mounted() && !tud_suspended() && !g_ejected.load();
}

}  // namespace drivers
