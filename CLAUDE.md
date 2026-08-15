# CLAUDE.md

Guidance for Claude Code working in this repository.

## What this is

Firmware for a motorcycle dashboard on a 2023 Honda CRF250F. Custom PCB,
ESP32-S3-WROOM-1 N16R8, 3.5" 320×480 ILI9488 over SPI, GPS, IMU, shift
lights, SD logging, BLE.

This is **v2 of the firmware, a clean rewrite.** The v1 Arduino sketch is
not in this repo and must not be copied. Read
`docs/system-architecture.md` before making structural changes — it is the
contract for how this code is organised, and it was argued through
deliberately.

**Hardware status: the PCB is at fab and has not been tested.** Nothing in
this repo has run on real hardware. Treat every driver as unproven.

## Build

Firmware (needs ESP-IDF v5.x on PATH):

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Host tests — **no ESP-IDF needed, this is the fast loop**:

```bash
cmake -S test -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Everything under `components/core/` builds in both. Keep it that way.

## The five rules

Do not break these without saying so explicitly.

1. **No globals.** A module gets data from its arguments. v1 had a
   `globals.h` with 60 externs; that is the thing being fixed.
2. **One writer per piece of state.** Producers own a `Published<T>` slot.
   Readers get a lock-free consistent copy. No mutexes for sensor data.
3. **`components/core/` never includes an ESP header.** No `esp_*.h`, no
   `freertos/*`, no `Arduino.h`. If you need time, take it as a parameter.
   This is what makes the logic testable on the host.
4. **A screen is a pure function of one `Frame`.** Screens do not read
   sensors, do not touch SPI, and do not call `get()` themselves. They
   receive a `Frame` and set widget values.
5. **A driver reports health; it does not decide policy.** Never `abort()`,
   never block forever, never retry in a tight loop.

## Layout

```
main/                  esp_main, task creation, dependency wiring
components/bsp/        pin map and bus setup — the only hardware truth
components/hal/        abstract interfaces (IEngine, IImu, IGnss, ...)
components/drivers/    one component per chip
components/core/       pure logic. NO esp headers. Builds on host.
components/services/   the tasks that glue drivers to core
components/ui/         canvas, widgets, screens, render task
test/                  host build, plain CMake + ctest
docs/                  architecture and hardware reviews
```

## Hardware truths

`components/bsp/include/bsp/pins.hpp` is generated from the shipped
netlist and is the only place pin numbers may appear. Do not hardcode a
GPIO anywhere else.

Things that will bite you:

- **TFT_RST is on the MCP23S17 (GPB6), not a GPIO.** Bring the SPI
  expander up before the panel. R3 pulls the line high, so a dead expander
  degrades rather than bricks the screen.
- **Neutral reads LOW.** Opto-isolated, pulled up on GPB5.
- **The handlebar button pod has no interrupt line.** It must be polled.
  100 Hz.
- **Pod I2C runs at 100 kHz, not 400 kHz.** 4.7k pull-ups over 0.5 m of
  cable give ~470 ns rise time. Fast mode allows 300 ns.
- **The MCP23S17 caps at 10 MHz** on the shared SPI bus. Give each device
  its own clock in its `spi_device_interface_config_t`.
- **Never call `spi_device_acquire_bus()`.** Three tasks share SPI2. The
  IDF driver serialises per-transaction on its own; acquiring the bus
  breaks that.
- **The BLE controller runs at priority 23**, above the real-time `ctrl`
  task. Both the controller and the NimBLE host must be pinned to core 1.
- **IO6 is the only spare pin with an ADC.** Do not spend it on something
  digital. A VIN sense divider needs it.
- **Firmware cannot tell bike power from USB power.** No VIN sense on this
  revision. See `docs/dash-v1-netlist-review.md` §3.
- **ILI9488 in SPI mode needs RGB666, 3 bytes per pixel.** It does not
  accept RGB565. Use the `esp_lcd_ili9488` component; do not write it.

## Core split

Core 0 is a real-time island: `ctrl` (500 Hz, shift lights), `imu`,
`gnss`, `logic`, `input`. No radio, no filesystem.
Core 1 absorbs what can be late: `render` (30 Hz), `logger`, `ble`,
`keeper`, plus the BLE controller and NimBLE host.

Tach edge to shift lamp must stay under 3 ms. If you add anything to
core 0 above priority 18, you have broken that.

## Conventions

- C++17. `snake_case` for functions and variables, `PascalCase` for types.
- One component per directory, each with its own `CMakeLists.txt`.
- Header guards: `#pragma once`.
- Public headers live in `include/<component>/`.
- No exceptions, no RTTI, no dynamic allocation after boot.
- Physical units in the name: `_ms`, `_ft`, `_mph`, `_deg`, `_g`.

## Testing

Every change to `components/core/` needs a test. The host loop is fast —
use it. `test/check.hpp` is a 40-line assertion harness so there is no
dependency to install; swap it for Catch2 if you want more.

## Current state

Scaffold only. What exists and is tested:

- `core/state/published.hpp` — the seqlock slot
- `core/nav/` — mode and screen state machine
- `core/shift/` — RPM to lamp pattern with hysteresis
- `core/geo/` — distance, bearing, finish-line crossing with sub-sample
  interpolation

Everything else in the architecture document is unwritten. Build order is
in `docs/system-architecture.md` §17. Steps 1, 3 and 4 need no hardware.

## Before the board arrives

Do not write drivers you cannot test. Write `core/` and the UI against a
fake `Frame`. The most valuable work right now is the lap timer, the
attitude filter, the trip computer, and every screen laid out against
static numbers.
