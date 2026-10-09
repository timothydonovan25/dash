// Keeps the module's ESP32-C6 on the ESP-Hosted firmware this host was
// built against, by pushing `demo_p4/c6_cp`'s image from the `slave_fw`
// partition over the SDIO link. The C6 has no USB of its own on this board,
// so this is the only way it is ever reflashed without a UART adapter on
// its test pads.
//
// Call after esp_hosted_connect_to_slave() and before the BT controller is
// brought up. Returns at once when the C6 is current. When it is not, it
// upgrades the C6 and restarts the whole dash, which only happens on the
// first boot after a flash that carried a newer C6 image.
#pragma once

namespace services {

void c6_firmware_update_if_needed();

// Pushes slave_fw to the C6 whatever its version says, and restarts the
// dash. For a C6 that refuses Wi-Fi: the BLE-only build and the BLE + Wi-Fi
// build carry the same ESP-Hosted version, so the version check cannot tell
// them apart -- only asking the C6 for Wi-Fi can. Returns only on failure.
void c6_firmware_push_now();

}  // namespace services
