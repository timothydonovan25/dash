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
    // The I2C0 bus from bsp (BusStatus::i2c0, an i2c_master_bus_handle_t).
    // Call before init(); recover_bus() replaces it with the new one.
    void attach(void* bus) { bus_ = bus; }

    hal::Status init() override;
    hal::Status read_buttons(uint8_t& pressed_mask) override;

    // Re-write the configuration registers. A pod that browns out over a
    // jump comes back with defaults and answers every read with 0xFF, which
    // reads as no buttons pressed — alive, and useless. input_service calls
    // this on a timer and after any run of NACKs (§12).
    hal::Status refresh_config();

    // Clock a stuck slave off the bus, then reset the I2C peripheral. For
    // when the bus itself is wedged rather than the device being confused.
    // Returns IoError if SDA was still held low afterwards.
    hal::Status recover_bus();

    // For the DIAG bus screen. `stuck` counts the recoveries that did not
    // free SDA, which is the difference between a pod that glitches and a
    // pod whose cable is shorted.
    uint16_t recoveries() const { return recoveries_; }
    uint16_t stuck() const { return stuck_; }
    uint8_t  last_clocks() const { return last_clocks_; }
    // GPIOA as last read, all eight pins (pressed = 1): the console's pod trace.
    uint8_t  last_gpa() const { return last_gpa_; }

private:
    hal::Status write_reg(uint8_t reg, uint8_t value);
    hal::Status read_reg(uint8_t reg, uint8_t& value);

    void*    bus_ = nullptr;      // i2c_master_bus_handle_t
    void*    dev_ = nullptr;      // i2c_master_dev_handle_t
    uint16_t recoveries_ = 0;
    uint16_t stuck_ = 0;
    uint8_t  last_clocks_ = 0;
    uint8_t  last_gpa_ = 0;
};

}  // namespace drivers
