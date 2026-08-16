// The shift lamps: eight bits on the expander, one PWM gate on IO16.
#pragma once
#include "hal/i_shift_lamps.hpp"

namespace drivers {

class Mcp23s17;

class ShiftLamps final : public hal::IShiftLamps {
public:
    explicit ShiftLamps(Mcp23s17* expander) : mcp_(expander) {}

    hal::Status init() override;
    hal::Status set_mask(uint8_t mask) override;
    hal::Status set_master(uint8_t percent) override;

private:
    Mcp23s17* mcp_;
    bool      ledc_ready_ = false;
    uint8_t   percent_ = 0;
};

}  // namespace drivers
