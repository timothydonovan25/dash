// Bus setup. The other half of "bsp is the only hardware truth".
//
// Pin numbers live in pins.hpp and buses are brought up here, so a driver
// never initialises a bus — it only adds a device to one. That matters most
// for SPI2, which three drivers share: whoever called `spi_bus_initialize`
// first would otherwise own the transfer size and the DMA channel for
// everybody else.
#pragma once

namespace bsp {

struct BusStatus {
    bool spi2  = false;   // IMU, SD card, MCP23S17
    bool spi3  = false;   // TFT only
    bool i2c0  = false;   // handlebar pod
    bool uart1 = false;   // GPS
};

// Idempotent: a bus that is already up reports true rather than failing, so
// the keeper can call this again after a peripheral reset.
BusStatus init_buses();

// Everything that has to be true before any driver runs: buses up, and every
// pin that drives something held at a safe level. Call once, first.
BusStatus board_init();

}  // namespace bsp
