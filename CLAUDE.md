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

Firmware. ESP-IDF **v6.0.2**, installed with EIM, so it is not on PATH
until you source its activation script:

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

v6 is not v5. It split the peripheral drivers into `esp_driver_*`
components, so `REQUIRES driver` no longer brings in UART or GPIO, and it is
stricter about `gpio_num_t` in C++ — a plain `int` pin number will not
convert on its own.

Simulator — the dashboard on a laptop, no hardware and no ESP-IDF:

```bash
cmake -S sim -B build-sim && cmake --build build-sim
python3 sim/dash_sim.py
```

It runs the firmware's own `ui::App`, screens, widgets, `logic_tick`,
button debounce and `shift::pattern()` against a fake bike, so what the
window shows is what the panel will show. Sliders for speed, rpm, lean and
the rest; the five buttons send down/up separately so the real debounce and
long-press timing decide what a press was. `build-sim/dash_sim` also speaks
a plain text protocol on stdin and emits PPM frames on stdout, so it can be
scripted and piped into ffmpeg.

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
components/dash_hal/   abstract interfaces (IEngine, IImu, IGnss, ...)
components/drivers/    one component per chip
components/core/       pure logic. NO esp headers. Builds on host.
components/services/   the tasks that glue drivers to core
components/ui/         canvas, widgets, screens, render task
test/                  host build, plain CMake + ctest
docs/                  architecture and hardware reviews
```

The architecture document calls the interface component `hal/`. It is
`dash_hal/` on disk because **ESP-IDF has its own component called `hal`**,
a component name is its directory name, and a project component wins. Naming
ours `hal` silently replaced the IDF's, and the build died three components
away with `fatal error: hal/efuse_hal.h: No such file or directory` inside
`espcoredump`. The include prefix and the namespace are both still `hal`, so
`#include "hal/i_imu.hpp"` is unchanged — only the directory moved.

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

The panel is the other budget. A whole 480x320 frame is 460 kB at the
ILI9488's 18-bit colour — 92 ms on the wire at 40 MHz, which is 11 fps no
matter how fast the drawing is. `render` therefore sends only the widgets
that changed, and **anything that moves must be a widget**: background
chrome is redrawn on a full repaint and never in between, so live data
painted there stops moving. `test_ui` checks that a partial update lands the
same pixels as a full repaint, which is the property that catches it.

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

Build-order steps 1, 3, 4 and the host-testable half of 8 are done.
20 host test suites, all passing.

**`core/`** — all host-tested:

- `state/` — the seqlock slot, the slot table, `Frame` + `snapshot()`, the
  `TrackBuffer` ring for bulk data, the `Command` type
- `nav/` mode and screen machine · `shift/` RPM to lamp pattern
- `geo/` distance, bearing, finish-line crossing with sub-sample interpolation
- `lap/` arm, score, best lap, and the speed / min-lap / fix-gap gates that
  turn a geometric crossing into a real lap
- `units/` conversions and all display formatting · `trip/` odometer,
  tripmeters, fuel model, engine hours
- `config/` the one ConfigItem table · `health/` DevState and the retry policy
- `attitude/` lean from `v·ω`, not from the accelerometer · `jump/` airtime
- `nmea/` the sentence parser, so the GPS driver is only bytes
- `input/` debounce and short/long/repeat · `tach/` pulses to rpm and mph
- `log/` the blackbox ring and its CSV format
- `gear/` nearest gear by the rpm/speed ratio, with the ratios in CONFIG
  because they depend on the sprockets and the rolling radius
- `delta/` lap delta against the session best — a **placeholder**, see below

**`dash_hal/`** — the abstract interfaces, including `NullEngine`.

**`ui/`** — canvas, 12 widgets, all 16 screens, the render loop, laid out
for a **landscape 480x320** panel. Builds and renders on the host; `test_ui`
draws every screen against a fake Frame. `ui/chrome.hpp` holds the shared
furniture — titles, rules, framed tiles — so fifteen screens do not each
invent their own idea of what a labelled box looks like.

**`sim/`** — the desktop simulator. `logic_tick.hpp` exists so it executes
the firmware's decisions rather than a copy of them: a simulator that
mirrors the code instead of running it drifts, and then stops being
evidence of anything.

**`bsp/`** — pins plus `buses.cpp`: SPI2, SPI3, I2C0 and UART1 are brought
up in one place, and a driver only ever adds a device to a bus. That matters
most for SPI2, which three drivers share.

**`drivers/`, `services/`, `main/`** — compile clean for esp32s3 against
IDF v6.0.2 and link into a 440 kB `dash.bin`. **Nothing has run.** Compiling
is not working: the drivers are written against datasheets and a netlist,
and the PCB is still at fab. See `components/drivers/README.md` for the
specific BRINGUP holes.

Nine tasks: ctrl, imu, gnss, logic, input on core 0; render, logger,
storage, keeper on core 1.

### What is deliberately not here

- **BLE** (`services/ble/`, `core/tpms/`) — step 9 of the build order, after
  the real-time path works, so a change in shift-light timing has one
  suspect. `sdkconfig.defaults` already pins the controller and host to
  core 1 for when it lands.
- **CAN / `engine_service`** — the board has no transceiver (§10.1).
  `NullEngine` is wired in and every screen renders `--` for the ECU.
- **USB mass storage** (§9.3) — the ownership seam is in
  `services/storage_service.cpp` and the TinyUSB half is not written. It
  cannot be a partial implementation: the device must unmount the card
  before a host touches it, or the FAT volume is corrupted by two writers.
- **A tuned lap delta.** `core/delta/` exists and works, but it is
  explicitly a placeholder: the reference is the session best held in RAM,
  matching is nearest-point on that trace within a window around the last
  match, and the number is quantised to a tenth at the source. Every
  threshold in `delta::Config` is a guess that has never seen a real GPS
  trace, and the tests around it are synthetic — they prove the matcher does
  what it was designed to do, not that the design suits real fixes. Tune the
  search window and `max_match_ft` on the first track day. A saved
  per-track reference is the next step and needs track identity, which is
  still open in §18.

Where files differ from the §3 sketch: `tach_capture` and `hall_capture` are
one `engine_sense` driver because they are the same three counters;
`backlight` is part of `ili9488` because it is one LEDC channel; `theme.hpp`
is the semantic layer over the palette in `geometry.hpp`.

## Before the board arrives

Do not write drivers you cannot test. Write `core/` and the UI against a
fake `Frame`. The most valuable work right now is the lap timer, the
attitude filter, the trip computer, and every screen laid out against
static numbers.
