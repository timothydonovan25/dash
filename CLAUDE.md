# CLAUDE.md

Guidance for Claude Code working in this repository. This file holds the
rules and the traps; the detail is in the docs mapped below. Read the one
doc your task needs, not all of them.

## What this is

Firmware for a motorcycle dashboard on a 2023 Honda CRF250F. Custom PCB
built around a Guition JC-ESP32P4-M3-C6 module (ESP32-P4 + ESP32-C6), a
3.5" 320×480 ILI9488 on an 8-bit 8080 parallel bus (used landscape
480×320), GPS, IMU, shift lights on a PCA9955B, SD logging, BLE, Wi-Fi.

This is **v2 of the firmware, a clean rewrite.** The v1 Arduino sketch is
not in this repo and must not be copied. `docs/system-architecture.md` is
the contract for how the code is organised; read it before structural
changes.

**Hardware status.** The P4 board was ordered 2026-10-07 and arrived
2026-10-28; bring-up is under way. Treat a driver as unproven until
`docs/bringup.md`'s status table says otherwise. In short: rails, boot,
panel, SD on 12 V, GPS (stuck at 1 Hz), IMU on the bench, the C6's SDIO
link (the Android app has bonded), and on the bench the tach, both wheel
sensors and the shift lamps have run; the pod on this board, anything at
speed on the bike, power-fail and SD-card OTA installs have not. The bike's
whole wiring harness is to be remade. The
current firmware version is in `firmware.txt`.

## Doc map

| Need | Read |
|---|---|
| New to the codebase: the whole firmware explained in depth (FreeRTOS use, every slot and Frame field and who writes it, every task, the UI pipeline, end-to-end flows, recipes for adding things) | `docs/firmware-guide.md` (long; read the part you need) |
| What a feature does, where its code is, how a rider reaches it; every CONFIG item and command | `docs/features.md` (index by code path at the top) |
| Why the code is shaped this way: rules, layers, slots and their writers, tasks, buses, boot order, UI model, storage, health, config | `docs/system-architecture.md` |
| BLE (GATT contract, iPhone, watch, pairing), Wi-Fi updates, partition table, stable/dev slots — architecture §11, same numbers. A bare "§11.x" in code means this file | `docs/radio-and-updates.md` |
| What has run on the board; bench procedure per subsystem; BRINGUP markers; `sdkconfig` traps | `docs/bringup.md` (status table first) |
| USB console commands, `pod_keys.py`, menu routes, soak summary | `docs/console.md` |
| A C6 with blank flash: flashing it over its UART | `docs/bringup.md` §9.2 |
| A chip's datasheet, and how this project uses it (pins, registers, values, traps) | `datasheets/` (`<PART>.pdf` + `<PART>-USAGE.md`, index in its README) |
| Pin numbers in code | `components/bsp/include/bsp/pins.hpp` (the only place) |
| Pins, nets, PCA9955B and rails, the pre-order audit, open hardware items | `docs/p4-board-pin-map.md` |
| Speed fusion design, dead wheels, replay | `docs/fusion.md` |
| IMU vibration plan (not built) | `docs/imu-vibration.md` |
| Offline OSM routing, turn-by-turn, the map builder (built; not run on the board) | `docs/navigation.md` |
| Custom screens and modes: the deck, the `.dui` format, the editor at `/screens`, why it is shaped so | `docs/custom-screens.md` (as built: `features.md` §36) |
| The Guition demo board build and its bring-up lessons | `demo_p4/README.md` |
| Release images and C6 images | `releases/README.md` |
| K-line ECU adapter (separate board, not ordered as far as the repo knows) | `kline-adapter/review.md` |
| Per-component notes | `components/*/README.md` |
| Superseded: S3 board, S3 respin, platform choice, first K-line adapter; `panel_test/` (an S3 panel test) | `docs/history/` — reasoning only, not current facts |
| Choosing a sunlight-readable panel; enclosure rules (ASA, no fan, gaskets, isolators) | `docs/history/p4-platform-selection.md` §4, §7 (the one live part of it) |

The Android companion apps are **not in this repo**: `~/phone/media-player` (the XP3900 player) and `~/phone/backpack` (an S26: the link without the player, plus the pod over `B`), each its own
Gradle/Kotlin tree. The only thing shared is the §11.1 GATT contract in
`docs/radio-and-updates.md`, which stays here.

## Build

**Run `fastfetch` before a serious task** to know the machine: the laptop is
`kao@thinkpad`, the desktop `Desktop`. Two separate clones of
`git@github.com:timothydonovan25/dash.git` (private, default `main`); work
moves by commit, push and pull — commit and push before switching machines,
`git pull` on arrival.

**Check for other Claude sessions in this clone before editing**
(`ListAgents`). If one is active, message it (`SendMessage`) with the files
you will touch and ask for its own; agree who owns a file before editing it.
Two sessions sharing `build/` or `sdkconfig`, or both bumping
`firmware.txt`, will clash: give one of them a worktree.

Host tests — **no ESP-IDF, the fast loop**:

```bash
cmake -S test -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Simulator — the firmware's own `ui::App`, screens, `logic_tick`, debounce
and `shift::pattern()` against a fake bike (features §29):

```bash
cmake -S sim -B build-sim && cmake --build build-sim
python3 sim/dash_sim.py
```

Firmware — ESP-IDF **v6.0.2** from EIM, not on PATH until sourced:

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

- On the desktop, EIM's `. ~/.espressif/tools/activate_idf_v6.0.2.sh`
  works as well as `export.sh`.
- `build/`, `build-host/`, `build-sim/` are per machine (gitignored). Python
  env: laptop `idf6.0_py3.14_env`, desktop `idf6.0_py3.12_env`. "*… is
  currently active in the environment while the project was configured
  with …*" is a tree configured on the other machine: `idf.py fullclean`.
- **`sdkconfig` is per machine and `sdkconfig.defaults` only seeds a new
  one.** `main.cpp` stops a stale one with an `#error` naming the setting:
  delete `sdkconfig` and rebuild (bringup §11).
- `idf.py coredump-info` needs ESP-IDF's GDB, which the laptop's
  `export.sh` leaves off PATH:
  `export PATH=~/.espressif/tools/riscv32-esp-elf-gdb/17.1_20260402/riscv32-esp-elf-gdb/bin:$PATH`.
- `/dev/ttyACM0` appears only once the hand-fitted P4 module (U16) is on.
- **v6 is not v5.** Peripheral drivers are split into `esp_driver_*`
  components (`REQUIRES driver` no longer brings in UART or GPIO), and C++
  is strict about `gpio_num_t`: a plain `int` pin will not convert.
- The C6's firmware: `idf.py -C demo_p4/c6_cp set-target esp32c6 build`,
  then `idf.py -p /dev/ttyACM0 c6-flash` puts it in `slave_fw`.

## The five rules

Do not break these without saying so explicitly.

1. **No globals.** A module gets data from its arguments.
2. **One writer per piece of state.** Producers own a `Published<T>` slot
   (two buffers, not a seqlock); readers get a lock-free consistent copy.
   No mutexes for sensor data.
3. **`components/core/` never includes an ESP header.** No `esp_*.h`, no
   `freertos/*`, no `Arduino.h`. Time comes in as a parameter. This is what
   makes the logic testable on the host.
4. **A screen is a pure function of one `Frame`.** Screens do not read
   sensors, touch a bus, or call `get()`.
5. **A driver reports health; it does not decide policy.** Never `abort()`,
   never block forever, never retry in a tight loop.

## Layout

```
main/                  esp_main, task creation, dependency wiring, USB console
components/bsp/        pin map and bus setup — the only hardware truth
components/dash_hal/   abstract interfaces (IEngine, IImu, IGnss, ...)
components/drivers/    one per chip
components/core/       pure logic, one directory per module. NO esp headers. Builds on host.
components/services/   the thirteen tasks that glue drivers to core
components/ui/         canvas, widgets, screens, the App
components/doom/ doomgeneric/ p4_restart_fix/
test/  sim/  tools/    host tests, simulator, host-side helpers
demo_p4/               the Guition demo board build, and c6_cp/ (the C6's firmware)
docs/                  see the doc map
releases/              every image built, in git
v2 pcb files/          the gerbers, BOM and pick-and-place JLCPCB built from
```

`dash_hal/` is the architecture's `hal/`: ESP-IDF has its own component
called `hal`, and a same-named project component silently replaces it (the
build dies in `espcoredump`). Include prefix and namespace are still `hal`.

## Hardware truths that bite

- **Pins only in `pins.hpp`.** Never hardcode a GPIO elsewhere. Where the
  drawn board and the plan disagree, `pins.hpp` follows the board. TFT D0..D7
  are GPIO33 down to 26 (reversed); RST 53, WR 52, DC 51, CS 50, TE 49.
- **The panel's FPC arrives flipped** into a top-contact connector: panel
  pin n lands on pad 41−n.
- **8-bit 8080, RGB565, two bytes per pixel. Not SPI.** Use
  `esp_lcd_ili9488` over an i80 IO handle; do not write the command set
  (except VIEWE's ST7365 table). The HT035 is **IPS**: the `_ips` init.
  `kTftWrHz` is 25 MHz, above the datasheet's 15.
- **The bike's P4 is silicon v3.2 (400 MHz), the demo's v1.3.** An image
  for one does not run on the other; each `sdkconfig.defaults` picks its own.
- **Backlight and shift lamps are one chip**, the PCA9955B on I2C1 at 1 MHz.
  A dead one is a dark screen *and* dark lamps. **`ctrl` is its only
  writer** (the level comes from `keeper` in `Deps::backlight_pct`); the one
  exception is `power` blanking it at VIN cut. Its reset is on the P4's EN,
  so a software reset does not reset it: `init()` rewrites every register.
- **Neutral reads LOW** (opto U6, GPIO12).
- **The pod has no interrupt line**: polled at 200 Hz (`kPodPollHz`).
  **I2C0 at 100 kHz**, not 400 (rise time over 0.5 m of cable), never shared
  with the PCA9955B.
- **SPI2 carries only the IMU and the SD card.** Each device its own clock.
  **Never call `spi_device_acquire_bus()`.** The IDF's SD host holds the bus
  for a whole SD command, busy time included; the IMU's FIFO absorbs it.
  **Every IMU transaction is polling**: a queued transmit beside the SD host
  deadlocked both and the watchdog rebooted the board.
- **The tach is an inductive wrap, 5–10 V.** The as-built R34/C19
  (100 kΩ / 470 pF) filtered its spike away; reworked by hand to 10 kΩ /
  47 pF, which reads on the bench (pin-map §6.3). A new board needs the
  same values.
- **VIN sense** (GPIO20) tells bike from USB; `ctrl` caps lamp duty on USB.
  **Test the SD card on 12 V**: on USB alone it did not mount.
- **The P4 has no radio.** BLE and Wi-Fi are the C6's, over internal SDIO
  with ESP-Hosted. The C6 is a MINI-1U and needs its external antenna.
  `start_ble` stays last in the boot order.
- **USB-C is the P4's USB-Serial/JTAG** (GPIO24/25). High-speed USB is not
  connected.

## Real-time and memory budgets

- **Core 0 is a real-time island**: `ctrl` (18, 500 Hz + every tach edge),
  `imu`, `gnss`, `logic`, `input`. No radio, no filesystem. Core 1 has
  `render`, `logger`, `storage`, `keeper`, `power`, `ota`, `ble`, `wifi`.
- **Tach edge to shift lamp under 3 ms.** Anything added to core 0 above
  priority 18 breaks it. ESP-Hosted's tasks and `sys_evt` are demoted at run
  time (`services::demote_radio_tasks`); NimBLE uses FreeRTOS timers
  (`CONFIG_BT_NIMBLE_USE_ESP_TIMER=n`, because `esp_timer` is at 22 on core
  0); lwIP is on core 1.
- **A device's re-init runs on the task that owns it**: `keeper` decides
  when (`claim_retry`) and sets a bit in `Deps::retry_requests`.
- **Task watchdog**: `ctrl`, `imu`, `logic`, `render` are fed once a loop
  and the bike panics after 5 s. Nothing added to those loops may block
  that long.
- **A flash write freezes core 0** (up to ~20 ms). Nothing writes flash
  while the bike moves: `keeper` and the bond store only when
  `core::flash::still`, OTA only parked, `power` at key-off.
- **The panel bus is the other budget.** A full frame is ~12 ms. `render`
  sends only what changed, through `ui::Shadow`. **Anything that moves must
  be a widget**: background chrome is redrawn only on a full repaint.
  Anything that leaves the glass unknown must `invalidate()` the shadow.
- **Internal RAM has a hard ceiling before the scheduler starts.** Big
  buffers go in PSRAM (`EXT_RAM_BSS_ATTR`) or on a task stack, never in a
  plain static. Check `nm build/dash.elf | grep _heap_start` after adding
  statics (features §28 has the numbers and the boot-loop story).

## Conventions

- C++17. `snake_case` functions and variables, `PascalCase` types.
- One component per directory, each with its own `CMakeLists.txt`; public
  headers in `include/<component>/`; `#pragma once`.
- No exceptions, no RTTI, no dynamic allocation after boot.
- **No stack frame over 1 kB**: `-Wframe-larger-than=1024 -Werror` on our
  components (`cmake/dash_stack.cmake`). Make big state a static, a member
  or PSRAM; a function that needs a big frame on purpose is wrapped in
  `DASH_BIG_FRAME_BEGIN`/`END` (`core/stack.hpp`) with the reason, and its
  task sized for it.
- Physical units in names: `_ms`, `_ft`, `_mph`, `_deg`, `_g`.
- **Settings have one writer at run time, `logic`**: everything else posts
  `ConfigSet`. CONFIG keys are permanent NVS IDs, never renumbered
  (`test_config` holds the shipped list).
- **A new screen** goes in `ScreenId`, `ui::AllScreens` and `App::build()`.
  `ScreenId`'s numbers are stored in `.dui` files: append, never reorder
  (`core::layout::Builtin` mirrors them, checked by `static_assert`).
- **Changed the Frame?** Run `python3 tools/frame_fields.py`: custom screens
  bind every Frame field by a permanent id, and ctest `frame_fields` fails
  until the generated table matches the headers.
- **Versions.** Bump `firmware.txt` (version + one-to-three-word feature)
  before a build you mean to keep; every build is copied to `releases/`.
- **Secure boot and flash encryption are off by decision**, and the
  partition table is at 0x10000; `main.cpp` refuses anything else.
- Wi-Fi is an end-user feature, not a debug channel: debug over USB.

## Logging is for RaceRender

Most logging exists so RaceRender can put the ride over the GoPro video.
The blackbox (`logs/*.csv`, first line `# RaceRender Data`) is lined up with
the video by time, so `Time` and `GpsUtc` matter more than any one channel.

- Anything that changes while riding is a blackbox column: one value per
  row, named with its unit, **appended at the end**. A name never changes
  and a column never moves: overlay templates map by name.
- A missing reading is written so a gauge visibly drops (0), never as the
  last value held.
- Per-event results go to the detail log's `_evt`; a column too only if it
  is worth showing while it happens. `detail/` files are for tuning, not
  RaceRender.

## Testing

Every change to `components/core/` needs a test (`test/test_<module>.cpp`;
`test/check.hpp` is a 40-line harness). Everything under `components/core/`
builds on the host and on the device; keep it that way.

## Away from the board

Do not write drivers you cannot test. Write `core/` and the UI against a
fake `Frame`, and check them in the sim.

## Keeping the docs indexable

When a change lands, update the doc that owns the fact, not this file:
behaviour in `features.md` (with its version), board results in
`bringup.md`'s status table, design reasons in `system-architecture.md`.
Add a line here only for a new rule or a new trap.
