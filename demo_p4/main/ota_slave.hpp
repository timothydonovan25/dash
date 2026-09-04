#pragma once

namespace demo {

// Pushes demo_p4/c6_cp's built firmware to the C6 over the existing SDIO
// link (ESP-Hosted's partition-OTA API) if the co-processor's reported
// version is older than what this host build expects, then reboots the C6
// and this host so both resync. No-op (returns immediately) if the running
// C6 firmware already matches. See docs/bringup-p4-demo.md — this exists
// because this board has only one USB-serial port (the P4's), so the C6
// can't be reflashed the way a UART-adapter bring-up normally would.
//
// Must be called after esp_hosted_connect_to_slave() and before anything
// that assumes the co-processor is at the version this host was built
// against (e.g. esp_hosted_bt_controller_init()).
void ota_slave_if_needed();

}  // namespace demo
