// The real handlebar pod (MCP23017) on the demo board's CN3 I2C connector.
//
// Not drivers::ButtonPod: that one recovers a wedged bus by deleting I2C0
// through bsp and clocking its pins by hand, and here the bus is the touch
// controller's too. (It was also written against the legacy driver/i2c.h
// API, which could not share a controller with i2c_master; both are
// i2c_master now, so that reason is gone.) This is the same three register
// writes and one read, with no recovery of its own. The
// pin-to-button mapping is core::input::pod_pins_to_buttons(), the same
// one the bike uses, so a button means the same thing on both.
//
// It is optional. With nothing on CN3 the touch zones and the serial
// buttons work as before, and plugging the pod in later is picked up within
// a second.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace demo {

class PodButtons {
public:
    // `bus` is TouchButtons::bus(). Ok if the pod answered; NotPresent if
    // nothing is plugged in, which is not an error on this board.
    hal::Status init(void* bus);

    // Bit 0 = A .. bit 4 = E, set means pressed. A pod that is missing or
    // stops answering reads as 0 and is reconfigured on a timer, so a pod
    // unplugged and plugged back in (and so back at its power-on defaults,
    // with IPOL off and every button reading as pressed) recovers by itself.
    //
    // The status is for the health table. Ok and every error are the
    // result of a real transaction; Busy means it made none this poll
    // (missing, waiting for the next retry), so there is nothing to report.
    hal::Status read_mask(uint8_t& mask_out, uint32_t now_ms);

    bool present() const { return configured_; }
    // GPIOA as last read, all eight pins after IPOL (pressed = 1), before
    // the pin-to-button map: for serial `inputs`. 0 when nothing was read.
    uint8_t last_gpa() const { return last_gpa_; }

private:
    hal::Status configure();

    void*    dev_ = nullptr;        // i2c_master_dev_handle_t
    bool     configured_ = false;
    uint32_t next_config_ms_ = 0;
    uint8_t  last_gpa_ = 0;
};

}  // namespace demo
