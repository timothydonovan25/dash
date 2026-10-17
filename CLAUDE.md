# CLAUDE.md

Guidance for Claude Code working in this repository.

## What this is

Firmware for a motorcycle dashboard on a 2023 Honda CRF250F. Custom PCB
built around a Guition JC-ESP32P4-M3-C6 module (ESP32-P4 + ESP32-C6), a
3.5" 320×480 ILI9488 on an 8-bit 8080 parallel bus, GPS, IMU, shift
lights on a PCA9955B, SD logging, BLE.

This is **v2 of the firmware, a clean rewrite.** The v1 Arduino sketch is
not in this repo and must not be copied. Read
`docs/system-architecture.md` before making structural changes — it is the
contract for how this code is organised, and it was argued through
deliberately.

**Hardware status: the P4 board was ordered on 2026-10-07** (JLCPCB,
top side assembled) **and has not arrived.** Nothing in this repo has run
on it. Treat every driver as
unproven. The audit is `docs/p4-board-pin-map.md` §6. The earlier ESP32-S3
board is superseded; its reviews stay in `docs/` as history.

## Build

Firmware. ESP-IDF **v6.0.2**, installed with EIM, so it is not on PATH
until you source its activation script:

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

**Two machines, two clones.** The desktop (`Desktop`) and a laptop are
separate clones of `git@github.com:timothydonovan25/dash.git` (private,
default branch `main`). Work moves between them by commit, push and pull —
uncommitted work does not follow you. Commit and push before switching
machines; `git pull` on arrival. (Until 2026-10-17 the folder was synced by
Syncthing, `.git` and `build/` included; that is gone.)

- `build/`, `build-host/` and `build-sim/` are in `.gitignore`, so each
  machine has its own. The laptop's ESP-IDF Python env is
  `idf6.0_py3.14_env`, the desktop's is `idf6.0_py3.12_env`. A build tree
  left over from the Syncthing days may still be configured by the other
  machine, and `idf.py build` stops with "*… is currently active in the
  environment while the project was configured with …*". That is not a
  broken install; the tree is now local, so `idf.py fullclean` on that
  machine fixes it.
- Both machines have ESP-IDF v6.0.2 from EIM. On the desktop the
  `export.sh` line above works as of 2026-10-16 (its Python env was
  installed with `idf_tools.py install-python-env`), and EIM's
  `. ~/.espressif/tools/activate_idf_v6.0.2.sh` works too.

The P4 module is hand-fitted after assembly, so expect `/dev/ttyACM0` only
once U16 is on. v6 is not v5. It split the peripheral drivers into `esp_driver_*`
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
long-press timing decide what a press was. Buttons for events no slider can
make (a jump, wheelspin and locks, a crank), and stand-ins that answer what
the firmware asks of tasks the sim does not run (ota, storage's eject, the
Wi-Fi hotspot, a BLE phone), so every alert and every DIAG state is
reachable: each device can be set ok, failing or absent, a wheel sensor
killed, the bike laid on its side, the GPS clock shifted to night (the
window dims by the backlight `keeper` would set). The bike reaches the
speed slider at 0.8 g / 1.1 g, never in one step. The FRAME line carries
the slip bar, the alert and the backlight for scripts. `build-sim/dash_sim` also speaks
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
docs/                  architecture, hardware reviews, bring-up
tools/                 host-side helpers, e.g. the BLE peripheral stand-in
v2 pcb files/          the gerbers, BOM and pick-and-place JLCPCB built from
```

The Android companion app is **not in this repo.** It lives in `~/phone`,
its own Gradle/Kotlin tree, because nothing here builds it and the only thing
the two share is the §11.1 GATT contract in `docs/system-architecture.md`.
That document is still the contract, and it stays here.

The architecture document calls the interface component `hal/`. It is
`dash_hal/` on disk because **ESP-IDF has its own component called `hal`**,
a component name is its directory name, and a project component wins. Naming
ours `hal` silently replaced the IDF's, and the build died three components
away with `fatal error: hal/efuse_hal.h: No such file or directory` inside
`espcoredump`. The include prefix and the namespace are both still `hal`, so
`#include "hal/i_imu.hpp"` is unchanged — only the directory moved.

## Hardware truths

`components/bsp/include/bsp/pins.hpp` is the only place pin numbers may
appear, and `docs/p4-board-pin-map.md` is the map behind it. Do not
hardcode a GPIO anywhere else. Where the drawn board and the original plan
disagree, `pins.hpp` follows the board.

Things that will bite you:

- **The TFT pins are as the board drew them, not as first planned.** D0..D7
  are GPIO33 down to GPIO26 (reversed); RST 53, WR 52, DC 51, CS 50, TE 49.
  The firmware follows the board.
- **The panel connector is top-contact and the panel's FPC arrives
  flipped.** The panel is on the bottom side, its tail wraps the right edge
  to FPC1 (AFC07-S40ECA-00), so panel pin n lands on pad 41−n.
- **The panel is 8-bit 8080, RGB565, two bytes per pixel.** Not SPI. IM[2:0]
  = 011 on the FPC. Use the `esp_lcd_ili9488` component over an i80 IO
  handle; do not write the command set.
- **The backlight and the shift lamps are one chip.** PCA9955B (U2) on its
  own I2C1 bus at 1 MHz: OUT0–7 are the lamps, OUT8–10 the backlight
  cathodes. A dead PCA9955B is a dark screen *and* dark lamps. **`ctrl` is
  its only writer**: lamps, backlight (level decided by `keeper`, handed over
  in `Deps::backlight_pct`), the once-a-second config refresh and the
  re-init. The one exception is `power` blanking it at VIN cut.
- **The PCA9955B's reset is on the P4's EN.** A software or USB-JTAG reset
  does not reset it, so `init()` must rewrite its registers.
- **Neutral reads LOW.** Opto-isolated (U6), pulled up by R64, on GPIO12.
- **The handlebar button pod has no interrupt line.** It must be polled.
  100 Hz.
- **Pod I2C runs at 100 kHz, not 400 kHz.** 4.7k pull-ups over 0.5 m of
  cable give ~470 ns rise time. Fast mode allows 300 ns. It is I2C0 and
  never shares with the PCA9955B.
- **SPI2 carries only the IMU and the SD card** (SD in SPI mode). Give each
  device its own clock. **Never call `spi_device_acquire_bus()`** — the
  IDF driver serialises per transaction on its own.
- **The tach is an inductive wrap on the plug lead, 5–10 V.** R34/C19 may
  filter its spike away; see `p4-board-pin-map.md` §6.3 before blaming
  firmware for a dead tach.
- **VIN sense exists on this board** (GPIO20, R17/R18), so firmware can
  tell bike power from USB. `ctrl` caps lamp duty on USB.
- **The P4 has no radio.** BLE and Wi-Fi live on the module's ESP32-C6, reached over
  internal SDIO with ESP-Hosted, as `demo_p4/` does. The C6 is a MINI-1U
  and needs an external antenna on its IPEX. Never call `start_ble` earlier
  in the boot order: it is last so a radio that will not come up delays nothing.
- **USB-C is the P4's USB-Serial/JTAG** (GPIO24/25): flashing, console and
  JTAG. The high-speed USB pins are not connected.

## Core split

Core 0 is a real-time island: `ctrl` (500 Hz, shift lights), `imu`,
`gnss`, `logic` (50 Hz, plus a pass whenever a fix or a command arrives:
`wake_logic`, `LogicPass`), `input`. No radio, no filesystem.
Core 1 absorbs what can be late: `render` (120 Hz), `logger`, `storage`,
`keeper`, `ota`, `ble` and the NimBLE host.

Tach edge to shift lamp must stay under 3 ms. If you add anything to
core 0 above priority 18, you have broken that. The tach ISR wakes `ctrl`
on every edge (`EngineSense::notify_on_tach`), so the lamps are decided at
the edge, not at the next 2 ms tick. ESP-Hosted's tasks and IDF's
`sys_evt` are created above 18 and are lowered at run time
(`services::demote_radio_tasks`, from main before `ctrl` starts and once a
second after); lwIP is pinned to core 1 in sdkconfig.

A device's re-init runs on the task that owns it, never on another core
under its transactions: `keeper` decides when (`claim_retry`) and sets a
bit in `Deps::retry_requests`; `imu`, `gnss`, `input` (the pod's bus),
`ctrl` (the LED driver) and `render` (the panel) do it.

**A flash write freezes core 0** for its whole length — up to ~20 ms when
NVS erases a page — because IDF suspends the other core's scheduler and
the P4 has no flash auto-suspend (IDF v6.0). So nothing writes flash while
the bike moves: `keeper` and NimBLE's bond store write only when
`core::flash::still` (every speed source below the line, engine at most
idling), OTA only parked, and `power` saves the totals at key-off (moving,
only once VIN has stayed down a few ms).

The panel is the other budget. A whole 480x320 frame is 307 kB in RGB565
— about 31 ms on the 8-bit bus at the 10 MHz starting write clock, four
whole periods of the 120 Hz render loop. `render` therefore sends only the widgets that
changed — and only the part that changed, where a widget can say
(`Widget::damage()`: a table's changed rows, the tach marker's rows), via
`ui::App::damage()`, which the sim and `test_ui` use too — and **anything that moves must be a widget**: background chrome
is redrawn on a full repaint and never in between, so live data painted
there stops moving. `test_ui` checks that a partial update lands the same
pixels as a full repaint, which is the property that catches it.

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

Build-order steps 1, 3, 4 and the host-testable half of 8 are done, and
the media half of step 9 is written. 40 host test suites, all passing.

**`core/`** — all host-tested:

- `state/` — the `Published<T>` slot, the slot table, `Frame` + `snapshot()`, the
  `TrackBuffer` ring for bulk data, the `Command` type. `Published<T>` is two
  buffers, not a seqlock: a seqlock reader that preempted its writer on the
  same core spun forever (ctrl at 18 reading gnss at 12). A reader never
  waits on a writer that has stopped half way
- `nav/` mode and screen machine · `shift/` RPM to lamp pattern, with an
  optional lead (CONFIG → Shift → Lead time, off by default): the lamps go by
  where the rpm will be, from `tach::RateEstimator`'s slope, less a noise floor
- `geo/` distance, bearing, finish-line crossing with sub-sample interpolation
- `lap/` arm, score, best lap, and the speed / min-lap / fix-gap gates that
  turn a geometric crossing into a real lap
- `units/` conversions and all display formatting · `trip/` odometer,
  tripmeters, fuel model, engine hours
- `config/` the one ConfigItem table, and its `group`s as CONFIG's submenus · `health/` DevState and the retry policy.
  Any task reports into the one `Tracker` through per-device atomics; only
  `keeper` (20 Hz) turns reports into states and publishes, on change, so the
  health slot has one writer. A report for an absent device is ignored: an
  ejected SD card stays ejected. A device that reports must `begin_init` first
- `attitude/` lean from `v·ω`, not from the accelerometer · `jump/` airtime
- `nmea/` the sentence parser, so the GPS driver is only bytes
- `input/` debounce and short/long/repeat; C and D act on the press
  (`on_press_mask`), as they have no long meaning — a button given one must leave the mask · `tach/` pulses to rpm and mph
- `log/` the blackbox ring, its CSV format, and the rate decimator
- `media/` the §11.1 Feed line parser: bounded, UTF-8-safe, `P` as an anchor
- `ota/` stable/dev slot roles from the bootloader's rollback state, the
  image header check, parked-only gating
- `ble/` which phone may connect: first bond free, then only in the
  pairing window; the 30 s encrypt deadline
- `sweep/` the key-on gauge sweep: rpm, speed and gear to full scale and
  back, applied to the Frame by `ui::App`, never to a slot
- `gear/` nearest gear by the rpm/speed ratio, with the ratios in CONFIG
  because they depend on the sprockets and the rolling radius
- `gearcal/` calibrating the gear table: `Window` finds steady stretches
  (rpm, speed and their ratio all still, GPS agreeing, in gear); `Guide` is
  CONFIG -> Gearing -> Calibrate gears, ten steady seconds per gear, told
  when to shift, results as % error before they are accepted; `Learner` runs
  the rest of the time and moves one entry by at most 1 % per power cycle,
  after eight agreeing episodes 30 s apart, never more than 5 % from the
  calibrated base (`gear_base_rpm_per_mph`). `logic` owns both and
  publishes `GearCal`; `ui/screen_gearcal.cpp` takes over CONFIG while a
  run is open. Road speed is `Motion::best_mph`, the same number
  `gear::estimate` divides by, so the table stays tied to the wheel it is
  measured on: set the wheel circumference first.
- `sun/` sun elevation from the GPS date, time and position, and the
  backlight dim curve behind CONFIG → Display → Dim at night. `Gnss` carries
  `day_of_year` from the RMC date because `utc_ms` is time of day only
- `alert/` which single alert the overlay shows (bike down, charging fault,
  dead wheel sensor, SD, lamps, IMU, GPS lost mid-lap), with onset delays,
  clear delays and a dismissal that holds until the fault genuinely clears.
  `ui::App` runs it on every Frame. Low fuel is deliberately not here: the
  burn model is a guess and there is no fuel-reset action to re-arm it
- `power/` also holds `FailDetector`: the VIN-cut trigger behind the
  `power` task (1 kHz, core 1): dark, stop SD writes, save the odometer.
  Hold-up is only a few ms, so it cannot flush a file. Never run on the board
  (bringup §8.1)
- `battery/` rest voltage, the crank dip and charging voltage, and a verdict
  (good / weak / not charging / overcharging). `power` feeds it at 100 Hz
  with the lowest VIN of each 10 ms, because a starter's dip is too short
  for keeper's 1 Hz, and owns the `battery` slot. DIAG → POWER shows it
- `slip/` wheelspin and lockup: each wheel against road speed (fused, GPS
  behind it), held 0.7 s at its peak. `ui::App` runs it on every Frame and
  draws a bar across the top of every screen (CONFIG → Display → Slip bar);
  the bar clearing is a full repaint, like a dismissed alert
- `waypoint/` the named waypoints START, TRUCK, CAMP, FUEL, MARK, the rule
  for when START moves (not set, or from another day, or 6 h old), and
  their NVS encoding
- `delta/` lap delta against the session best — a **placeholder**, see below
- `fusion/` stage 1 of sensor fusion: road speed from the IMU, both
  wheels and GPS Doppler, gated, with GPS refereeing the wheels.
  `docs/fusion.md`; `tools/replay` runs it over a log or a synthetic ride.
  `WheelCheck` faults a wheel sensor that reads 0 for 5 s at speed, into
  the health table as FrontWheel / RearWheel, and `ctrl` skips it
- `wifi/` the update hotspot's policy: idle and moving shut-off, the
  session password, which log names the page may serve

**`dash_hal/`** — the abstract interfaces, including `NullEngine`.

**`ui/`** — canvas, 20 widgets, all 17 screens, the render loop, laid out
for a **landscape 480x320** panel. Builds and renders on the host; `test_ui`
draws every screen against a fake Frame. `ui/chrome.hpp` holds the shared
furniture — titles, rules, framed tiles — so fifteen screens do not each
invent their own idea of what a labelled box looks like.
**`ui::App` keeps one screen alive at a time** (1.17): it is built into one
slot sized for the largest (`ui::AllScreens`) when entered and destroyed
when left, so the App is ~1.9 kB on render's stack, not 13 kB, and a new
screen costs nothing unless it becomes the largest. Add a new screen to
`AllScreens`, `ScreenId` and `App::build()`. Nothing a screen keeps survives
leaving it; what must (map traces, lap history, settings, CONFIG's place
during gear calibration) the App holds and hands back in `build()`.

**`sim/`** — the desktop simulator. `logic_tick.hpp` exists so it executes
the firmware's decisions rather than a copy of them: a simulator that
mirrors the code instead of running it drifts, and then stops being
evidence of anything.

**`bsp/`** — pins plus `buses.cpp`: SPI2, I2C0 (pod), I2C1 (PCA9955B),
UART1 (GPS) and the i80 LCD bus are brought up in one place, and a driver
only ever adds a device to a bus.

**`drivers/`, `services/`, `main/`** — compile clean for esp32p4 against
IDF v6.0.2 and link into a 1.09 MB `dash.bin` (NimBLE and esp_hosted are about 430 kB of it). **Nothing has run.** Compiling
is not working: the drivers are written against datasheets and a netlist,
and the board has not arrived yet. See `components/drivers/README.md` for the
specific BRINGUP holes.

Thirteen tasks: ctrl, imu, gnss, logic, input on core 0; render, logger,
storage, keeper, power, ota, ble, wifi on core 1.

**Stable and dev firmware are built, and have never run on the board**
(architecture §11.7). The two 6 MB app slots hold a stable and a dev image;
which is which is read off the bootloader's rollback state, never stored. A
dev image never confirms itself, so any reset returns to stable. `ota`
installs `update/dash.bin` from the card into the dev slot and boots dev
once; promoting is manual only, from inside dev via DIAG → Update → Promote
to stable. DIAG → Update drives all of it; power-on always boots stable,
with no chooser, so nothing slows the boot. `tools/flash_dev.py` writes the dev slot over USB; `idf.py
flash` makes a new stable; the Wi-Fi page (below) uploads into the dev slot.
**Rollback must be in the
first bootloader flashed** — `main.cpp` will not build without it — and the
bootloader has about 1.1 kB left.

**Versions.** `firmware.txt` is the version and a one-to-three-word feature.
Every build of either board is copied to `releases/` as
`dash-<version>-<date>-<board>-<feature>.bin`, read back out of the image, and
`releases/` is in git. Bump `firmware.txt` before a build you mean to keep.
The Update screen shows version and date for both slots, and an upload built
for the other board (`demo` vs `bike`) is refused.

**Wi-Fi updates are built, and have run on the demo board, not the bike.**
An on-demand WPA2 hotspot (`CRF250F-Dash`, a new eight-digit password each
time, shown on DIAG → Update), never a station. `http://192.168.4.1` uploads
a `dash-*.bin` to the dev slot, downloads and deletes ride logs, downloads
firmware backups, and edits every CONFIG setting (as `ConfigSet` commands,
which `logic` applies).
It turns off after 10 idle minutes or when the bike moves. The upload goes
through `Deps::ota_stream` to `ota`, which stays the one writer of the dev
slot with the same checks as the card; nothing on the network can boot an
image. A C6 without Wi-Fi in its firmware has it replaced from `slave_fw`.
Architecture §11.7.1. **Watch internal RAM:** the SDIO link, lwIP and NimBLE
share it, and `wifi` logs what is free at every change of state.
**Internal `.bss`/`.data` has a hard ceiling before that.** Until the
scheduler starts, the only internal heap is from `_heap_start` up to
0x4FF3AFC0 plus 18 kB; the 384 kB region at 0x4FF40000 is added after.
ESP-Hosted's constructor took ~87 kB of it for SDIO buffers, so with
`_heap_start` at 0x4FF21280 the demo could not create its idle tasks and
boot-looped (`vApplicationGetIdleTaskMemory` assert). Those buffers are now
in PSRAM (`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`; measured on the demo,
2026-10-14: 38 kB internal allocated at constructor time instead of ~125 kB,
131 kB free). ESP-Hosted falls back to internal RAM silently if PSRAM
fails, so if this ever boot-loops again, check that first. Check
`nm build/dash.elf | grep _heap_start` after adding statics; the bike is at
0x4FF1E450. Big buffers go on a task stack (allocated later) or in PSRAM
with `EXT_RAM_BSS_ATTR`, never in a plain static.

**CRUISE → NAV** guides back to one of five named waypoints:
`core::geo::guide` gives distance and bearing, heading-up above 3 mph and
north-up below. The list shows every waypoint's distance; the row the cursor
is on is the target, and it opens on START, which `logic` sets at the first
good fix of a ride (a key-off at lunch keeps it). Short E puts the selected
one here, long E clears it. `logic` owns them; `keeper` saves them to NVS
while stopped and `logic_restore` brings them back.

**Ride logs** are `logs/sNNN.csv` until the GPS gives a date, then renamed to
the local date, `logs/2026-10-13_s007.csv` (core::log::log_file_name; the
session number is one past the highest on the card). Each row ends with
`AltFt`, GPS altitude. The Wi-Fi page exports any log as a GPX track,
converted in the page's own JavaScript so the dash does no work. DIAG →
System shows the P4's die temperature (`drivers::ChipTemp`, keeper, 1 Hz)
and the IMU's, from its FIFO packets.
**Startup** shows a 1.5 s splash before the gauge sweep (CONFIG → Startup).

**SD eject.** CONFIG → SD card → Eject flushes and unmounts; DIAG → System
shows "safe to pull" until the card comes out. One lock in
`drivers/sdcard` guards every card call from `logger`, `ota`, `storage` and
`wifi`.

**BLE media is built, and has never run on this board.** `services/src/ble_service.cpp`
brings up ESP-Hosted to the C6, NimBLE on core 1, and the §11.1 Dash Media
service (Feed in, Ctrl out). The phone must bond: Feed needs an encrypted
link, and `core/ble/` decides which phone may (first one freely, then only
in CONFIG's pairing window; Forget phone clears them). NimBLE's callbacks only queue;
`ble` is the one writer of `media` and `ble_diag`, and the line parser is
`core/media/`, host-tested. ESP-Hosted makes its SDIO/RPC tasks at
priority 23 with no core affinity and no Kconfig for either, so `ble`
lowers them to 8 by name. **If esp_hosted is upgraded, check the boot log
still says it found them.** It also pushes `demo_p4/c6_cp`'s firmware to the C6
from `slave_fw` when the C6 is older than 2.12; build that project first.

### What is deliberately not here

- **TPMS** (`core/tpms/`, the observer scan) — waits on a sensor to
  capture (§11.2, §18); the observer role is off in sdkconfig until then.
  The *phone* half is written, in its own repo at
  `~/phone` — along with `tools/dash_media_peripheral.py`, which stayed
  here: a laptop peripheral speaking §11.1 so the app has a working
  counterpart and the dash will later have a known-good client. Neither end
  has talked to the other.
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
`backlight` is three PCA9955B channels that `ili9488` forwards to; `theme.hpp`
is the semantic layer over the palette in `geometry.hpp`.

## When the board arrives

`docs/bringup.md` is the procedure: what to do in what order, what you
should see, and what each failure means. Read §0 before applying power —
the FPC pin check and the partition table are both irreversible — and do
§1's rail check before hand-fitting U16 and U4. F1 needs its 1 A fuse.

## Before the board arrives

Do not write drivers you cannot test. Write `core/` and the UI against a
fake `Frame`. The most valuable work right now is the lap timer, the
attitude filter, the trip computer, and every screen laid out against
static numbers.
