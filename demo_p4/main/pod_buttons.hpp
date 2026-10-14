// The real handlebar pod (MCP23017) on the demo board's CN3 I2C connector.
//
// Not drivers::ButtonPod: that one is written against the legacy
// driver/i2c.h API on the real board's I2C0, and the touch controller here
// already owns this bus through the new i2c_master driver. The two APIs
// cannot share a controller (IDF aborts at boot if both are linked in), so
// this is the same three register writes and one read over i2c_master. The
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

private:
    hal::Status configure();

    void*    dev_ = nullptr;        // i2c_master_dev_handle_t
    bool     configured_ = false;
    uint32_t next_config_ms_ = 0;
};

}  // namespace demo
