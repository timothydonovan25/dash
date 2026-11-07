// The SD card as a USB drive on the laptop, over the dash's USB-C
// (docs/console.md, features §9.3).
//
// The USB-C is wired to the P4's USB-Serial/JTAG (the console, flashing).
// That peripheral can only be a serial port, so the drive is TinyUSB on the
// P4's full-speed OTG, swapped onto the USB-C's PHY (PHY 0) only while the
// drive is up; USB-Serial/JTAG goes to PHY 1 with its pads off (PHY 1's pads
// are the panel's D6/D7, GPIO26/27, and must stay the panel's). While it is
// up there is no console and no flashing over USB. stop() swaps back; a
// boot calls restore_phy() first thing, so a reset or a crash in drive mode
// always comes back with the console, and a power cycle always does.
//
// The caller (`storage`) unmounts the dash's own filesystem first: the card
// has one owner at a time, the dash or the laptop, never both.
#pragma once
#include "hal/status.hpp"

namespace drivers {

class UsbDrive {
public:
    // The USB-C back to USB-Serial/JTAG. Safe at any time; first in app_main.
    static void restore_phy();

    // Brings the card up on its own (no filesystem) and presents it to the
    // laptop. The dash's SdCard must be unmounted. Undoes itself on failure.
    hal::Status start();
    // The drive away, the console back. The caller remounts the card.
    void stop();

    bool active() const { return active_; }
    // The laptop has the drive (enumerated it); false before and after.
    bool host_connected() const;

private:
    bool  active_ = false;
    void* storage_ = nullptr;   // tinyusb_msc_storage_handle_t
    int   dev_ = -1;            // sdspi_dev_handle_t
    bool  usb_ = false;         // the TinyUSB driver is installed
};

}  // namespace drivers
