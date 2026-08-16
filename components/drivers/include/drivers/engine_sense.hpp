// Tach, wheel sensors and the neutral switch.
//
// The tach is an inductive pickup wrapped around the ignition lead through a
// 100k series resistor, a 1M bias and a BAT54S clamp. **There is no filter
// capacitor on this revision**, so one spark produces a burst of edges, not
// one edge. The refractory window below is what turns that burst back into a
// single count, and it is the single most important number in this file.
//
// Each channel's ISR timestamps its edge. core/tach needs that: at 500 Hz
// polling and 6000 rpm the pulses are 10 ms apart, and timing them by the
// poll interval instead reads 30000 rpm.
#pragma once
#include "hal/i_engine.hpp"

namespace drivers {

class Mcp23s17;

class EngineSense final : public hal::IEngineSense {
public:
    // The neutral switch is on the expander, not on a GPIO, so this driver
    // reads it through the one that owns it.
    explicit EngineSense(Mcp23s17* expander) : mcp_(expander) {}

    hal::Status init() override;
    hal::Status read(Counts& out) override;

    void set_refractory_us(uint32_t tach_us) { tach_refractory_us_ = tach_us; }

private:
    Mcp23s17* mcp_;
    uint32_t  tach_refractory_us_ = 0;
    bool      started_ = false;
};

}  // namespace drivers
