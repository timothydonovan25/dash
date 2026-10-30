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
    // The two I2C masters, as i2c_master_bus_handle_t (the IDF's i2c_master
    // driver; the legacy driver/i2c.h API is gone from this firmware).
    // Null if the bus did not come up. A driver adds its device to one; the
    // SCL speed is the device's, from pins.hpp.
    void* i2c0 = nullptr;  // handlebar pod, 100 kHz
    void* i2c1 = nullptr;  // PCA9955B LED driver, 1 MHz
    bool uart1 = false;   // GPS
    // The 8080 bus for the panel, an esp_lcd_i80_bus_handle_t, with WR and
    // DC as the HT035 wants them. Creating it twice makes a second bus on
    // the same pins rather than failing, so only board_init() creates it,
    // and only the panel's probe swaps it, by releasing it first.
    void* lcd_bus = nullptr;
};

// Idempotent: a bus that is already up reports true rather than failing.
// Does not touch the LCD bus.
BusStatus init_buses();

// I2C0 alone, for the pod's bus recovery to hand the pins back. Not
// init_buses(): that reconfigured I2C1 under `ctrl`'s lamp writes and put
// the GPS UART back to its boot baud rate, so a pod glitch over a jump
// cost the GPS until the next power cycle. Returns the bus (the one that
// is up, if it is), or null.
void* init_pod_bus();

// Takes I2C0 down so the pod's recovery can clock its pins by hand. The
// pod's device must already be removed from it. init_pod_bus() brings it
// back, as a new handle.
void release_pod_bus();

// The 8080 bus, with WR and DC traded for the UEED035HV if `swap_wr_dc`
// (pins.hpp). For the panel's probe alone, which owns the bus and must
// release_lcd_bus() the old one first. Returns the handle, or null.
void* init_lcd_bus(bool swap_wr_dc);
void release_lcd_bus(void* bus);

// Everything that has to be true before any driver runs. Call once, first.
BusStatus board_init();

}  // namespace bsp
