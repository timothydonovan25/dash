// The two port expanders, and the button pod behind one of them.
//
// MCP23S17 on the board (SPI2, 10 MHz ceiling) and MCP23017 in the handlebar
// pod (I2C0, 100 kHz — 4.7k pull-ups over half a metre of cable give about
// 470 ns of rise time, and fast mode allows 300). Same register map, two
// transports, one interface.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IExpander {
public:
    virtual ~IExpander() = default;
    virtual Status init() = 0;

    virtual Status read_port(uint8_t port, uint8_t& value) = 0;
    virtual Status write_port(uint8_t port, uint8_t value) = 0;
    virtual Status set_direction(uint8_t port, uint8_t input_mask) = 0;
    virtual Status set_pullups(uint8_t port, uint8_t mask) = 0;

    // Re-write every configuration register without a full re-init.
    //
    // A browned-out expander comes back with default registers: it still
    // acknowledges, it still returns data, and every pin it was driving is
    // an input again. It looks completely alive and does nothing at all.
    // `input_service` calls this periodically for exactly that reason (§12).
    virtual Status refresh_config() = 0;
};

// The handlebar pod: five buttons on an MCP23017.
//
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
