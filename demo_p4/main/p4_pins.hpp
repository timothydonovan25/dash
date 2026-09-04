// p4_pins.hpp — the only place a pin number may appear in this demo.
//
// Unlike `components/bsp/include/bsp/pins.hpp`, none of this is verified
// against a netlist: it's a generic AliExpress dropship board with no
// published schematic. Every value below is a placeholder for the ESP32-P4
// MIPI-DSI + I2C touch layout that boards in this class commonly use.
// Confirm each one against the real board (continuity from the module's
// pins to the display FPC / touch header) before trusting it — see
// docs/bringup-p4-demo.md.
#pragma once
#include <cstdint>

namespace p4pins {

// MIPI-DSI is a fixed differential bus on the ESP32-P4 (GPIO 33-40 on most
// boards), so there's normally nothing to configure here beyond lane count.
inline constexpr int kDsiDataLanes = 2;

// kPanelReset is unused (p4_panel.cpp passes GPIO_NUM_NC — the published
// firmware for this exact board doesn't drive a reset pin for this panel).
// kPanelBacklight=23 matches that same published firmware
// (BSP_LCD_BACKLIGHT in Guition JC4880P443C's esp32_p4_function_ev_board
// BSP fork) — our own on-device backlight scan was ambiguous between 23
// and 26 (they're adjacent in the scan's cycle and the flash times the
// user read off a clock could fit either within normal reaction-time
// error), so this real published source is what settled it.
inline constexpr int kPanelReset     = 27;    // unused, see above
inline constexpr int kPanelBacklight = 23;

// Touch (GT911, the common controller for this panel size/resolution) on
// its own I2C bus. UNCONFIRMED.
inline constexpr int kTouchSda   = 7;
inline constexpr int kTouchScl   = 8;
inline constexpr int kTouchRst   = 22;
inline constexpr int kTouchInt   = 21;
// I2C bus speed itself is fixed at 100kHz by the vendor macro
// ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() in touch_buttons.cpp, not here.

// The panel's native resolution as manufactured (portrait). The board is
// physically rotated to landscape for this demo, so the driver swaps these
// when it talks to the DSI/DPI panel and reports width()/height() in the
// UI's own landscape terms instead (see p4_panel.hpp).
inline constexpr int kPanelNativeW = 480;
inline constexpr int kPanelNativeH = 800;

}  // namespace p4pins
