// Five on-screen touch zones standing in for the real button pod.
//
// The panel has no physical buttons, so five equal strips down the left
// margin (the space p4_panel.cpp leaves outside the centered 480x320 UI)
// are drawn as A-E and read back as taps. Reading a raw 5-bit mask and
// handing it to core::input::Decoder is deliberate: input_service.cpp
// (components/services/src/input_service.cpp) does exactly this with the
// real MCP23017 pod, so debounce, long-press and repeat behave identically
// here without a line of new logic — only a new way of producing the mask.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace demo {

class TouchButtons {
public:
    hal::Status init();

    // Bit 0 = A .. bit 4 = E, set means "finger down in that zone". Reads
    // the controller once; core::input::Decoder does the debouncing, so
    // this does not need to.
    hal::Status read_mask(uint8_t& mask_out);

    // The I2C bus init() made (an i2c_master_bus_handle_t), or null if it
    // failed. The pod on CN3 hangs off the same wires; see p4_pins.hpp.
    void* bus() const;

private:
    void* io_ = nullptr;      // esp_lcd_panel_io_handle_t
    void* tp_  = nullptr;     // esp_lcd_touch_handle_t
};

}  // namespace demo
