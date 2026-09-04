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

// GPIO5, 22, 21, 23 all confirmed against elik745i/ESP32-2432S024C-Remote's
// boards/jc4880p443c_i_w.json + src/jc4880p443_bringup_main.cpp — a
// bring-up sketch written specifically for this exact board variant
// (JC4880P443C_I_W), not a guess or a same-family reference like the
// esp32_p4_function_ev_board BSP was. That sketch pulses this reset pin
// low-then-high before anything else works, which we were never doing
// (an earlier guess had assumed no reset pin existed at all).
inline constexpr int kPanelReset     = 5;
inline constexpr int kPanelBacklight = 23;

// Touch (GT911) on its own I2C bus. SDA/SCL confirmed against Espressif's
// esp32_p4_function_ev_board header; RST/INT confirmed against the
// jc4880p443c_i_w bring-up sketch above.
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
