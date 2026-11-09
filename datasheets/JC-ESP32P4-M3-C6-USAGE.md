# JC-ESP32P4-M3-C6 (U16) — usage

Guition module, 27 × 27 mm, 80 castellated pins, **hand-fitted** after JLC
assembly (not in the BOM). Inside: ESP32-P4NRW32 (32 MB PSRAM in package,
16 MB flash) and an ESP32-C6-MINI-1U-N4 joined by internal SDIO.

Datasheets: `ESP32-P4.pdf` (chip), `ESP32-P4-TRM.pdf` (registers),
`ESP32-C6-MINI-1U.pdf` (the radio module). No module datasheet exists; the
pinout comes from Guition's dev-board schematic, transcribed in
`docs/p4-board-pin-map.md` §2.

## Silicon revision
- The bike board's P4 is **v3.2 (400 MHz)**; the Guition demo board's is
  v1.3. Images are not interchangeable — each `sdkconfig.defaults` picks its
  own. The datasheet here is the current (v3.x) edition.

## Pins
All pin numbers live in `components/bsp/include/bsp/pins.hpp`; never
elsewhere. Pins nothing may use (pin-map §1):
- GPIO14–19: internal SDIO to the C6.
- GPIO6: C6_IO2 on Guition's board.
- GPIO39–48: VDD_IO_5 domain, set by firmware.
- GPIO35/36: boot straps. GPIO37/38: UART0, ROM console.
- GPIO24/25: USB-Serial/JTAG (the USB-C). High-speed USB is not connected.
- GPIO34 (LED_SDA) is the JTAG-source strap; harmless with its pull-up.

## Peripherals used
| Peripheral | Use |
|---|---|
| LCD i80 (8-bit) | Panel, `esp_lcd` i80 IO + `esp_lcd_ili9488` |
| SPI2 | IMU + SD card. Never `spi_device_acquire_bus()`; IMU transactions polling only |
| I2C0, 100 kHz | Handlebar pod (MCP23017) |
| I2C1, 1 MHz | PCA9955B |
| UART1 | PA1616D GPS |
| PCNT | Tach and wheel pulses (glitch filter + `kTachRefractoryUs`) |
| ADC1_CH4 (GPIO20) | VIN sense, bike vs USB |
| SDIO slave link | C6 via ESP-Hosted (BLE, Wi-Fi) |

## The C6
- Reset on GPIO54 (10 k + 1 µF on the board).
- Came with **blank flash**: flash it over its UART (`docs/bringup.md` §9.2),
  then updates go via `idf.py ... c6-flash` (`slave_fw`).
- The MINI-1U needs its external antenna on the IPEX; weak signal at
  bring-up was traced to the antenna (bringup §9).
- ESP-Hosted's tasks are demoted at run time so core 0 stays real-time
  (`services::demote_radio_tasks`).

## Traps
- Internal RAM ceiling before the scheduler starts: big buffers in PSRAM
  (`EXT_RAM_BSS_ATTR`).
- A flash write freezes core 0 for up to ~20 ms; nothing writes flash while
  moving.
- ROM SHA fault boot loop after a panic (`components/p4_restart_fix`).
- Module is rated to 70 °C ambient — the thermal limit of the whole board.
