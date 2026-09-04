// The BLE side of the real dash's own contract — docs/system-architecture.md
// §11.1 "Dash Media": one write-no-response GATT characteristic, one line
// per write, tag character then value (T/A/L/S). This is the first thing
// in the repo to actually run that contract; components/services/ble/ has
// never been built (step 9 of the build order — see CLAUDE.md).
//
// The radio itself is the ESP32-C6 co-processor: the P4 has no radio of
// its own, so NimBLE runs on the P4 as normal and talks to a BLE
// controller running on the C6 over SDIO via ESP-Hosted's VHCI transport
// (esp_hosted_bt_host_stack_setup()). See demo_p4/c6_cp/ for the
// co-processor's own firmware and docs/bringup-p4-demo.md for the pin
// wiring and the C6 slave-firmware version this needed.
#pragma once
#include "core/state/vehicle.hpp"

namespace demo {

// Brings up the hosted BT controller binding, NimBLE, and the Dash Media
// GATT service, then starts advertising. Call once from app_main.
void ble_media_start();

// A thread-safe copy of whatever the phone last wrote — zeroed if nothing
// has connected yet, or if the last connection dropped (§11.1: "on
// disconnect, ble must publish a zeroed Media").
core::state::Media ble_media_snapshot();

}  // namespace demo
