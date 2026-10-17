// Bus setup. The other half of "bsp is the only hardware truth".
//
// Pin numbers live in pins.hpp and buses are brought up here, so a driver
// never initialises a bus — it only adds a device to one. That matters most
// for SPI2, which two drivers share: whoever called `spi_bus_initialize`
// first would otherwise own the transfer size and the DMA channel for
// everybody else.
#pragma once

namespace bsp {

struct BusStatus {
    bool spi2  = false;   // IMU, SD card
    bool i2c0  = false;   // handlebar pod, 100 kHz
    bool i2c1  = false;   // PCA9955B LED driver, 1 MHz
    bool uart1 = false;   // GPS
    // The 8080 bus for the panel, an esp_lcd_i80_bus_handle_t. Only
    // board_init() creates it, because creating it twice makes a second
    // bus on the same pins rather than failing.
    void* lcd_bus = nullptr;
};

// Idempotent: a bus that is already up reports true rather than failing.
// Does not touch the LCD bus.
BusStatus init_buses();

// I2C0 alone, for the pod's bus recovery to hand the pins back. Not
// init_buses(): that reconfigured I2C1 under `ctrl`'s lamp writes and put
// the GPS UART back to its boot baud rate, so a pod glitch over a jump
// cost the GPS until the next power cycle.
bool init_pod_bus();

// Everything that has to be true before any driver runs. Call once, first.
BusStatus board_init();

}  // namespace bsp
