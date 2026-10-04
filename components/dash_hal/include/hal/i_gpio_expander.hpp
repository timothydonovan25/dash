// The handlebar button pod: five buttons on an MCP23017 over I2C0.
//
// 100 kHz, not 400 — 4.7k pull-ups over half a metre of cable give about
// 470 ns of rise time, and fast mode allows 300. The P4 board has no
// expander of its own any more; this file keeps its name because the pod
// is still one.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

// This connector has no interrupt line, so it must be polled at 100 Hz.
// Debounce belongs to the service; this reports what the pins say now.
class IButtonPod {
public:
    virtual ~IButtonPod() = default;
    virtual Status init() = 0;
    // Bit per button, bit 0 = A. A set bit means pressed regardless of the
    // wiring polarity — the driver inverts, and nothing above it has to
    // remember which way round the switches are.
    virtual Status read_buttons(uint8_t& pressed_mask) = 0;
};

}  // namespace hal
