// The eight shift lamps.
//
// On the P4 board they are constant-current outputs 0–7 of a PCA9955B, so a
// lamp's brightness is its PWM duty and nothing else: no series resistor,
// no master FET, and no dependence on the battery voltage.
//
// `set_master` is still the brightness control, and it still has to be: on
// USB the whole board shares a 500 mA port, so `ctrl` caps it when the VIN
// sense says the bike is not the supply.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IShiftLamps {
public:
    virtual ~IShiftLamps() = default;
    virtual Status init() = 0;
    // Bit 0 is lamp 1, straight from core::shift::pattern().
    virtual Status set_mask(uint8_t mask) = 0;
    // 0 turns everything off however the mask is set. This is the only way
    // the lamps are ever lit.
    virtual Status set_master(uint8_t percent) = 0;
};

}  // namespace hal
