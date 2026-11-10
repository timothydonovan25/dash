// Tach, wheel sensors and the neutral switch.
//
// The tach is an inductive pickup wrapped around the ignition lead through a
// 10k series resistor (R34), a 1M bias to GND, a BAT54S clamp and 47 pF (C19,
// a ~0.5 µs filter: too fast to stop ringing), so one spark produces a burst
// of edges, not one edge. The refractory window below is what turns that burst back into a
// single count, and it is the single most important number in this file.
//
// Each channel's ISR timestamps its edge. core/tach needs that: at 500 Hz
// polling and 6000 rpm the pulses are 10 ms apart, and timing them by the
// poll interval instead reads 30000 rpm.
#pragma once
#include "hal/i_engine.hpp"

namespace drivers {

class EngineSense final : public hal::IEngineSense {
public:
    hal::Status init() override;
    hal::Status read(Counts& out) override;

    void set_refractory_us(uint32_t tach_us) { tach_refractory_us_ = tach_us; }

    // A FreeRTOS task (TaskHandle_t) to notify on every tach edge that gets
    // past the refractory window: `ctrl`, so a new rpm reaches the lamps at
    // the edge rather than at its next 2 ms tick. At most one edge per
    // refractory window, so a noisy lead cannot wake it more than ~250/s.
    // nullptr stops it.
    static void notify_on_tach(void* task);

private:
    uint32_t  tach_refractory_us_ = 0;
    bool      ready_ = false;     // every channel installed; read() says so
};

}  // namespace drivers
