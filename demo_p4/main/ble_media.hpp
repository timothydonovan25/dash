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
#include "core/health/health.hpp"
#include "core/state/vehicle.hpp"

namespace demo {

// Brings up the hosted BT controller binding, NimBLE, and the Dash Media
// GATT service, then starts advertising. Call once from app_main.
//
// Reports Dev::BleHost (the stack itself) and Dev::Phone (whether one is
// currently connected) to `health`, which is what makes DIAG's Radio
// screen say anything real instead of showing the tracker's untouched
// Absent default.
void ble_media_start(core::health::Tracker& health);

// A thread-safe copy of whatever the phone last wrote — zeroed if nothing
// has connected yet, or if the last connection dropped (§11.1: "on
// disconnect, ble must publish a zeroed Media").
core::state::Media ble_media_snapshot();

// §11's reserved "Ctrl" characteristic: notify the phone of a transport
// command. `cmd` is 0 previous, 1 play/pause, 2 next — anything else is
// dropped, same "unknown values are ignored" convention as the inbound
// tags. A no-op if nothing is connected or nothing has subscribed yet.
void ble_media_send_ctrl(uint8_t cmd);

// What DIAG's Radio screen (§11.6) shows about the link itself, as opposed
// to what it's carrying (that's ble_media_snapshot()). Queries RSSI fresh
// each call — it costs nothing (NimBLE just returns the last received
// packet's cached RSSI) so there's no reason to cache it here too.
core::state::BleDiag ble_media_diag();

}  // namespace demo
