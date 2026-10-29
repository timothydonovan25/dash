// bsp/pins.hpp — the ONLY place a pin number may appear.
//
// P4 board: Guition JC-ESP32P4-M3-C6 module (U16). The assignment and the
// reasoning behind it are in docs/p4-board-pin-map.md, which is the
// contract this file follows. **Nothing here is verified against a
// netlist yet** — the P4 board has not been drawn. When it has, check every
// line below against it the way the S3 file was checked against
// `FInal_v1.net`.
//
// Module pin numbers are U16 castellations; the GPIO numbers are what the
// code uses. If you find yourself typing a raw GPIO number anywhere else in
// this repo, stop and add it here instead.
//
// Pins nothing may use (pin-map §1): GPIO14–19 are the module's internal
// SDIO link to the C6; GPIO6 is labelled C6_IO2 on Guition's dev board;
// GPIO39–48 are powered from VDD_IO_5, which firmware sets; GPIO35/36 are
// the boot straps; GPIO37/38 are UART0, the ROM console.
#pragma once
#include <cstdint>

namespace bsp {

// ── Shared SPI bus (SPI2): IMU and SD card ───────────────────────────
// Through the GPIO matrix, which is comfortable at 24 MHz. Two tasks use
// this bus. The IDF spi_master driver serialises transactions per bus, so
// this is safe — but NEVER call spi_device_acquire_bus(), which defeats it.
inline constexpr int kSpiMosi        = 8;    // U16-12
inline constexpr int kSpiSclk        = 9;    // U16-13
inline constexpr int kSpiMiso        = 10;   // U16-14
inline constexpr int kCsImu          = 7;    // U16-11, ICM-42688-P
inline constexpr int kCsSd           = 11;   // U16-15, DM3AT-SF card socket

inline constexpr int kSpiHzImu       = 24'000'000;
inline constexpr int kSpiHzSd        = 20'000'000;

// ── Display: ILI9488 on an 8-bit 8080 bus ────────────────────────────
// RGB565, two bytes per pixel, IM[2:0] = 011 on the FPC. "FPC n" below is
// the PANEL's pin n. The board connector (FPC1, AFC07-S40ECA-00, top
// contact) takes the panel's tail flipped, after it wraps round the board
// edge, so panel pin n lands on connector pad 41-n.
//
// Everything below follows the board as drawn (netlist 2026-10-06 rev 5),
// not the first plan in p4-board-pin-map.md §2: the layout reversed the
// data bus and rotated the control pins, and the firmware follows the
// copper rather than the copper following the plan. The i80 bus takes any
// GPIO for any bit, so a reversed bus costs nothing.
inline constexpr int kTftData[8]     = {33, 32, 31, 30, 29, 28, 27, 26};  // D0..D7 = U16-52..45 → FPC 17..24
inline constexpr int kTftRst         = 53;   // U16-73 → FPC 15, R3 pull-up
inline constexpr int kTftWr          = 52;   // U16-72 → R30 22 Ω → FPC 11
inline constexpr int kTftDc          = 51;   // U16-71 → FPC 10
inline constexpr int kTftCs          = 50;   // U16-70 → FPC 9, R21 pull-up
inline constexpr int kTftTe          = 49;   // U16-69 ← FPC 8 FMARK, optional
inline constexpr int kTftWidth       = 320;
inline constexpr int kTftHeight      = 480;
// The ILI9488's write cycle is about 66 ns, so 15 MHz is the datasheet's
// ceiling, but the first board's panel was clean at 25 and at 30 MHz once it
// had the IPS init sequence (2026-10-28). Held at 25: one step of margin
// below the highest seen clean. Its speckle before that was the init
// sequence, at every clock from 5 to 25 MHz, not the bus. If a panel ever
// speckles hot or on the bike, this is the first thing to lower.
inline constexpr int kTftWrHz        = 25'000'000;
// The largest single transfer: one band of the render task's band buffer,
// 40 landscape lines. The bus and the panel driver's staging buffer are
// both sized from this.
inline constexpr int kTftBandLines   = 40;
inline constexpr int kTftMaxBlitPx   = kTftHeight * kTftBandLines;

// ── GPS: PA1616D on UART1 ────────────────────────────────────────────
// PPS is deliberately not wired: the on-screen clock comes from RMC, lap
// crossings are interpolated between fixes, and every t_ms here is the
// P4's own timer.
inline constexpr int kGpsRx          = 2;    // U16-5, ESP RX <- GPS TX
inline constexpr int kGpsTx          = 3;    // U16-6, ESP TX -> GPS RX
inline constexpr int kGpsBootBaud    = 9600;
inline constexpr int kGpsRunBaud     = 57600;
// When the UART's interrupt hands received bytes on (buses.cpp): this many
// in the FIFO, or the line idle for this many character times.
inline constexpr int kGpsRxFullBytes = 8;
inline constexpr int kGpsRxIdleChars = 2;

// ── IMU ──────────────────────────────────────────────────────────────
// Data ready. The imu task blocks on this rather than polling, and falls
// back to a timeout if no edge comes; DIAG → System counts how often.
inline constexpr int kImuInt1        = 13;   // U16-20

// ── Vehicle inputs ───────────────────────────────────────────────────
// Tach is an inductive wrap around the spark plug lead, 5-10 V at most,
// via 100k series (R34) / 1M bias (R43) / BAT54S clamp (D2) / 470 pF (C19).
// That RC may swallow a short spike; see p4-board-pin-map.md §6.3. Expect
// ringing too: enforce a refractory window in firmware.
inline constexpr int kTachSig        = 21;   // U16-22
inline constexpr int kTachRefractoryUs = 1500;   // tune on the bench
inline constexpr int kWheelHallFront = 22;   // U16-23
inline constexpr int kWheelHallRear  = 23;   // U16-24

// Opto-isolated, pulled up by R64. **Reads LOW in neutral.**
inline constexpr int  kNeutral          = 12;   // U16-19
inline constexpr bool kNeutralActiveLow = true;

// CARD1-9 with R16's pull-up. LOW means a card is in.
inline constexpr int  kSdCardDetect          = 1;   // U16-4
inline constexpr bool kSdCardDetectActiveLow = true;

// ── VIN sense ────────────────────────────────────────────────────────
// VIN → 100k (R17) → pin → 22.1k (R18) → GND, 100 nF and a 3.3 V clamp at the pin. ADC1.
// About 0.8 V at the pin on USB, 2.6 V on a running bike.
inline constexpr int   kVinSense       = 20;   // U16-21, ADC1_CH4
inline constexpr float kVinTopKohm     = 100.0f;
inline constexpr float kVinBottomKohm  = 22.1f;

// ── Handlebar button pod: MCP23017 over I2C0 ─────────────────────────
// 100 kHz, NOT 400 kHz. 4.7k pull-ups over 0.5 m of cable give roughly
// 470 ns rise time; fast mode allows 300 ns. No interrupt line: polled.
inline constexpr int kI2cSda         = 4;    // U16-8
inline constexpr int kI2cScl         = 5;    // U16-9
inline constexpr int kI2cHz          = 100'000;
inline constexpr uint8_t kPodAddr    = 0x20;
// 200 Hz: a press is seen 5-10 ms after it lands (two matching reads, see
// core::input::Config::debounce_ms), not 10-20. Each read is ~0.45 ms of a
// bus nothing else is on.
inline constexpr int kPodPollHz      = 200;

// ── LED driver: PCA9955B on I2C1 ─────────────────────────────────────
// Its own bus at 1 MHz (Fm+, 2.2k pull-ups), never the pod's: a crushed pod
// cable must not take the shift lamps with it, and a lamp write must not
// queue behind a pod read. GPIO34 is the JTAG-source strap, read only when
// an eFuse nobody has burnt says so; the pull-up holds it high harmlessly.
inline constexpr int kLedSda         = 34;   // U16-53
inline constexpr int kLedScl         = 0;    // U16-76
inline constexpr int kLedI2cHz       = 1'000'000;
inline constexpr uint8_t kLedAddr    = 0x15;   // AD2,AD1,AD0 = GND,VDD,GND

// REXT = 1.91 kΩ: full scale is 255 × 0.225 / 1.91 = 30.0 mA on every
// output. That is both the lamps' chosen maximum and the panel's specified
// 30 mA per backlight pin, so IREF 255 is in spec for both. These ceilings
// are hardware facts and nothing above the driver can raise them.
// The lamps are OSRAM LR T67F.01 (LCSC C37981853): 50 mA max up to an
// 85 °C solder point, derating to 30 mA at 100 °C (datasheet v1.2, p.10),
// so 30 mA is inside the rating across the whole operating range.
inline constexpr int     kLedLamp0        = 0;    // OUT0..OUT7 = lamps 1..8
inline constexpr int     kLedLampCount    = 8;
inline constexpr int     kLedBacklight0   = 8;    // OUT8..OUT10 = LEDK1..3
inline constexpr int     kLedBacklightCount = 3;
inline constexpr uint8_t kLedLampIrefMax      = 255;   // 30 mA
inline constexpr uint8_t kLedBacklightIrefMax = 255;   // 30 mA per string

// USB BENCH LIMIT: eight lamps at 30 mA plus the backlight plus the P4 is
// close to a 500 mA port. `ctrl` caps the lamps at this duty when the VIN
// sense says the supply is USB.
inline constexpr int kShiftDutyUsbMax = 96;   // of 255

// ── ESP32-C6 on the module ───────────────────────────────────────────
// Reached over the module's internal SDIO when the BLE build lands
// (`esp_hosted`, as demo_p4 does). Nothing in this build talks to it.
inline constexpr int kC6Reset        = 54;   // U16-74, 10k + 1 µF on the board

// ── USB, boot and debug ──────────────────────────────────────────────
// Fixed-function, used by the ROM and the IDF rather than by anything
// here. Listed so nobody assigns them to something else.
//   U16-39/40  high-speed USB OTG, not connected
//   U16-42/43  GPIO24/25, USB-Serial/JTAG: the USB-C connector, via D3.
//              Flashing, the console and JTAG debug all go through it.
//   U16-54     GPIO35, BOOT button; download mode needs GPIO36 high too
//   U16-55     GPIO36, 10k pull-up
//   U16-56/57  GPIO37/38, UART0 console (test pads)
//   U16-75     CHIP_PU, the reset button

}  // namespace bsp
