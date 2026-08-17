// bsp/pins.hpp — the ONLY place a pin number may appear.
//
// Verified against the shipped netlist `FInal_v1.net` (167 parts, 110
// nets) on 2026-08-15. Module pin numbers are ESP32-S3-WROOM-1 package
// pins; the GPIO numbers below are what the code uses.
//
// If you find yourself typing a raw GPIO number anywhere else in this
// repo, stop and add it here instead.

#pragma once
#include <cstdint>

namespace bsp {

// ── Shared SPI bus (SPI2): IMU, SD card, MCP23S17 ────────────────────
// Three tasks use this bus. The IDF spi_master driver serialises
// transactions per bus, so this is safe — but NEVER call
// spi_device_acquire_bus(), which defeats it.
inline constexpr int kSpiMosi        = 11;
inline constexpr int kSpiSclk        = 12;
inline constexpr int kSpiMiso        = 13;

inline constexpr int kCsImu          = 14;   // ICM-42688-P
inline constexpr int kCsSd           = 21;   // DM3AT-SF card socket
inline constexpr int kCsMcp          = 47;   // MCP23S17

// Per-device clock ceilings. The MCP23S17 is the slow one.
inline constexpr int kSpiHzImu       = 24'000'000;
inline constexpr int kSpiHzSd        = 20'000'000;
inline constexpr int kSpiHzMcp       = 10'000'000;   // hard limit, datasheet

// ── Display bus (SPI3): ILI9488, its own bus, no contention ──────────
// The panel needs RGB666 (3 bytes/px) in 4-line SPI mode. It does NOT
// accept RGB565. Use the esp_lcd_ili9488 component.
inline constexpr int kTftSclk        = 9;
inline constexpr int kTftMosi        = 10;
inline constexpr int kTftMiso        = 40;
inline constexpr int kTftCs          = 48;
inline constexpr int kTftDc          = 7;
inline constexpr int kTftBacklight   = 15;   // LEDC PWM
// TFT reset is NOT a GPIO — see kMcpTftRst below.

inline constexpr int kTftWidth       = 320;
inline constexpr int kTftHeight      = 480;

// ── GPS: PA1616D on UART1 ────────────────────────────────────────────
inline constexpr int kGpsRx          = 4;    // ESP RX  <- GPS TX
inline constexpr int kGpsTx          = 5;    // ESP TX  -> GPS RX
// Wired, and deliberately unused. Nothing needs it: the clock on screen is
// whatever the last RMC said and does not care about a few tens of
// milliseconds, lap crossings are interpolated between fixes by core/geo
// rather than timed off a pulse, and every t_ms in the firmware is
// esp_timer since boot. It would earn its place for disciplining the
// oscillator or timestamping in absolute UTC, and neither is wanted here.
inline constexpr int kGpsPps         = 18;
inline constexpr int kGpsBootBaud    = 9600;
inline constexpr int kGpsRunBaud     = 57600;

// ── IMU ──────────────────────────────────────────────────────────────
// Data ready. The imu task blocks on this rather than polling, so a FIFO
// read gets exactly the samples that have arrived instead of racing the
// ODR. It falls back to a timeout if no edge comes, and DIAG → System
// counts how often that happens.
inline constexpr int kImuInt1        = 8;

// ── Vehicle inputs ───────────────────────────────────────────────────
// Tach is an inductive pickup wrapped around the ignition lead, via a
// 100k series / 1M bias / BAT54S clamp. There is no filter capacitor on
// this revision, so expect ringing: enforce a refractory window in
// firmware and do not trust raw edge counts.
inline constexpr int kTachSig        = 17;
inline constexpr int kTachRefractoryUs = 1500;   // tune on the bench

inline constexpr int kWheelHallFront = 1;    // ADC1_CH0, used as digital
inline constexpr int kWheelHallRear  = 2;    // ADC1_CH1, used as digital

// ── Shift lamps ──────────────────────────────────────────────────────
// Eight lamps on the MCP23S17 GPA port, plus a master low-side FET (Q11)
// driven by this PWM for global brightness.
//
// USB BENCH LIMIT: all eight lamps plus backlight plus the 3V3 rail draws
// roughly 610 mA from the 5 V rail, against a 500 mA USB 2.0 port. Clamp
// the duty when there is no engine RPM.
inline constexpr int kShiftMasterPwm = 16;
inline constexpr int kShiftDutyUsbMax = 96;   // of 255

// ── Handlebar button pod: MCP23017 over I2C ──────────────────────────
// 100 kHz, NOT 400 kHz. 4.7k pull-ups over 0.5 m of cable give roughly
// 470 ns rise time; fast mode allows 300 ns, standard mode allows 1000 ns.
// CN1 carries SDA/SCL/VCC/GND only — there is no interrupt line, so the
// pod must be polled.
inline constexpr int kI2cSda         = 41;
inline constexpr int kI2cScl         = 42;
inline constexpr int kI2cHz          = 100'000;
inline constexpr uint8_t kPodAddr    = 0x20;
inline constexpr int kPodPollHz      = 100;

// ── USB and debug ────────────────────────────────────────────────────
inline constexpr int kUsbDminus      = 19;   // dedicated S3 USB pins
inline constexpr int kUsbDplus       = 20;
inline constexpr int kDebugRx        = 44;   // header H1
inline constexpr int kDebugTx        = 43;

// ── MCP23S17 port assignments (not GPIOs) ────────────────────────────
namespace mcp {
inline constexpr int kShift0         = 0;    // GPA0..GPA7 = lamps 1..8
inline constexpr int kShiftCount     = 8;
inline constexpr int kNeutral        = 13;   // GPB5 — reads LOW in neutral
inline constexpr int kTftRst         = 14;   // GPB6
inline constexpr int kSdCardDetect   = 15;   // GPB7
// GPB0..GPB4 (8..12), INTA and INTB are free.
inline constexpr bool kNeutralActiveLow = true;
}  // namespace mcp

// ── Spare pins ───────────────────────────────────────────────────────
// IO6 is the ONLY spare on an ADC (ADC1_CH5). IO1 and IO2 are ADC1
// channels already spent on the wheel Halls. Reserve IO6 for the VIN
// sense divider the next board revision needs.
inline constexpr int kSpareAdc       = 6;
inline constexpr int kSpareDigital0  = 38;
inline constexpr int kSpareDigital1  = 39;
// IO3, IO45, IO46 are strapping pins — usable, but must be at their
// default level at reset.
// IO35, IO36, IO37 are octal PSRAM on the N16R8. DO NOT USE.

}  // namespace bsp
