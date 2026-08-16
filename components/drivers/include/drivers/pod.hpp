// The handlebar button pod: an MCP23017 on the end of half a metre of I2C.
//
// Two constraints from the netlist review, both of which will bite if
// ignored:
//
//  - **100 kHz, not 400.** The 4.7k pull-ups over that cable give about
//    470 ns of rise time. Fast mode allows 300 ns; standard mode allows
//    1000. At 400 kHz this works on the bench and fails in the rain.
//  - **No interrupt line.** CN1 carries SDA, SCL, VCC and GND only, so the
//    pod is polled at 100 Hz. There is nowhere to hang an ISR.
#pragma once
#include "hal/i_gpio_expander.hpp"

namespace drivers {

class ButtonPod final : public hal::IButtonPod {
public:
    hal::Status init() override;
    hal::Status read_buttons(uint8_t& pressed_mask) override;

    // Re-write the configuration registers. A pod that browns out over a
    // jump comes back with defaults and answers every read with 0xFF, which
    // reads as no buttons pressed — alive, and useless. input_service calls
    // this on a timer and after any run of NACKs (§12).
    hal::Status refresh_config();

    // Reset the I2C peripheral itself, for when the bus is stuck low rather
    // than the device being confused.
    hal::Status recover_bus();

private:
    hal::Status write_reg(uint8_t reg, uint8_t value);
    hal::Status read_reg(uint8_t reg, uint8_t& value);

    bool installed_ = false;
};

}  // namespace drivers
