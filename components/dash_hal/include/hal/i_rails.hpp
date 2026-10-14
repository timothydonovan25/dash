// The supply rails, as the board can measure them.
//
// One divider on VIN today. The driver reports the voltage at the ADC pin;
// turning that into volts at the connector, and into "bike or USB", is
// core::power's job, so it can be tested on the host.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IRailSense {
public:
    virtual ~IRailSense() = default;
    virtual Status init() = 0;
    // Millivolts at the VIN sense pin, calibrated.
    virtual Status read_vin_pin_mv(uint32_t& mv) = 0;
    // The same, but one conversion instead of an average and quick enough to
    // run every millisecond. Noisier; for watching the supply fall, not for
    // showing it.
    virtual Status read_vin_pin_mv_fast(uint32_t& mv) { return read_vin_pin_mv(mv); }
};

}  // namespace hal
