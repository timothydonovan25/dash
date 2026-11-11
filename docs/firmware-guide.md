# The CRF250F Dash Firmware — A Complete Guide

**Who this is for.** You know C++ reasonably well: classes, templates,
references, `std::atomic`, what a vtable is. You know nothing about this
project, nothing about the ESP32, and nothing about motorcycles. By the end
you should be able to open any file in `components/`, say what it is for,
who calls it, what data it reads, what data it writes, and what would break
if you changed it.

**How it relates to the other docs.** This is the long, linear explanation.
The other docs are references you look things up in:

| Doc | Use it for |
|---|---|
| `CLAUDE.md` | The rules and traps, one screen long |
| `docs/features.md` | What each feature does from the rider's side; every CONFIG item |
| `docs/system-architecture.md` | The original design argument |
| `docs/radio-and-updates.md` | BLE, Wi-Fi, partitions, firmware slots (architecture §11) |
| `docs/bringup.md` | What has actually run on the hardware |

Facts here are as of firmware **1.62** (2026-11-02). Where code and this
guide disagree, the code wins; every claim below names the file to check.

---

## Contents

- Part 1 — Orientation: what the device is, the hardware, the toolchain
- Part 2 — The five rules and the runtime you are working in (cores, memory)
- Part 3 — How FreeRTOS is used: every primitive, where, and why
- Part 4 — Shared state: `Published<T>`, the slot table, the `Frame`, every field
- Part 5 — The wiring: `Deps`, the command queue, request slots, `Settings`, health
- Part 6 — Boot, step by step
- Part 7 — Every task, in depth
- Part 8 — The `core/` modules
- Part 9 — The UI: canvas, widgets, screens, the App, the damage pipeline
- Part 10 — End-to-end flows, traced through the code
- Part 11 — Persistence: NVS, the SD card, flash partitions
- Part 12 — Testing, the simulator and the tools
- Part 13 — Recipes: how to add each kind of thing
- Part 14 — Pitfalls, and a glossary

---

# Part 1 — Orientation

## 1.1 What the device is

A dashboard for a 2023 Honda CRF250F, an off-road motorcycle with no
dashboard of its own (no speedometer, no tachometer, nothing). The dash
bolts to the handlebar and shows:

- **Speed**, from magnets and sensors on both wheels, from GPS, and from a
  sensor-fusion filter that combines them with an accelerometer.
- **Engine rpm**, from a wire wrapped around the spark-plug lead (each spark
  induces a pulse), and **shift lights**: eight LEDs that light up in
  sequence as rpm climbs, telling the rider when to change gear.
- **Lean angle, pitch, jumps (airtime), wheelies**, from an IMU (an inertial
  measurement unit: a gyroscope plus an accelerometer on one chip).
- **Lap timing** on a race track, from GPS: set a finish line, and the dash
  times every lap, splits it into sectors, and shows how far ahead or
  behind the best lap you are ("delta").
- **Trip computer**: odometer, two trip meters, estimated fuel use, engine
  hours.
- **Phone integration over Bluetooth**: the song playing, previous/next
  buttons, call and message notifications; the rider's **heart rate** from
  a watch.
- **A Wi-Fi hotspot** with a web page for firmware updates, downloading ride
  logs and editing settings.
- **A data logger**: every ride is written to an SD card as a CSV file that
  a video-editing program (RaceRender) overlays on GoPro footage.

The rider controls it with a five-button pod on the handlebar (buttons A to
E). There is no touchscreen.

## 1.2 What "firmware" means here

There is no operating system in the desktop sense. The program is one
binary, `dash.bin` (about 1.8 MB), written to the chip's flash memory. At
power-on the chip's ROM runs a bootloader, the bootloader runs our program,
and our program runs until the power goes. It uses **FreeRTOS**, a small
real-time kernel that gives you threads (called *tasks*), queues,
semaphores and timers, and **ESP-IDF**, Espressif's SDK of drivers and
libraries for its chips. There is no `main()`: ESP-IDF calls
`extern "C" void app_main()` (in `main/main.cpp`) from a task it created,
and when `app_main` returns that task is deleted while the tasks it started
carry on.

## 1.3 The hardware, for a software person

The custom circuit board ("the P4 board", or "the bike board") is built
around one module bought ready-made: the **Guition JC-ESP32P4-M3-C6**. That
module carries two chips:

- **ESP32-P4** — the main processor. Two RISC-V cores at 400 MHz, 32 MB of
  PSRAM (external RAM in the same package) and 16 MB of flash. **It has no
  radio.** Everything in this repo's firmware runs here.
- **ESP32-C6** — a smaller chip with Wi-Fi and Bluetooth. It runs
  Espressif's stock firmware ("ESP-Hosted") which makes it behave like a
  network card for the P4. The two talk over **SDIO**, the same bus an SD
  card uses. Our code never runs on the C6; it only sends it a firmware
  image when the C6's is out of date (`core/c6`, `services/src/c6_firmware.cpp`).

Around the module, on the board:

| Part | What it does | How the P4 reaches it |
|---|---|---|
| 3.5" ILI9488 LCD, 480×320 landscape | The screen | 8-bit parallel "8080" bus: 8 data lines + write strobe + data/command + chip select |
| PCA9955B | 16-channel constant-current LED driver: outputs 0–7 are the shift lamps, 8–10 the screen's backlight | I²C bus 1 at 1 MHz |
| ICM-42688-P | The IMU: 3-axis gyro + 3-axis accelerometer, with a 2 kB FIFO | SPI bus 2 |
| microSD card | Logs, saved tracks, update images | SPI bus 2 (shared with the IMU) |
| PA1616D | GPS receiver; speaks NMEA text sentences | UART 1 (serial) |
| MCP23017 in the handlebar pod | 16-bit I/O expander; five of its pins are the buttons | I²C bus 0 at 100 kHz, over a 0.5 m cable |
| Tach input | The spark-lead wrap, through a filter, to a GPIO | GPIO 21, edge interrupts |
| Wheel sensors (front, rear) | Hall-effect switches that pulse as magnets on the wheel pass | GPIO 22 / 23, edge interrupts |
| Neutral switch | The gearbox's neutral contact, through an optocoupler | GPIO 12, **LOW in neutral** |
| VIN sense | A resistor divider from the 12 V supply to an ADC pin | GPIO 20 (ADC1) |
| USB-C | The P4's built-in USB-Serial/JTAG: flashing, a console, debugging | GPIO 24/25 |

**Every pin number lives in one file**, `components/bsp/include/bsp/pins.hpp`.
No other source file may contain a GPIO number. `docs/p4-board-pin-map.md`
is the hardware reference behind it.

A few hardware facts shape the software so heavily that you need them
before anything else:

1. **The backlight and the shift lamps are one chip.** If the PCA9955B dies,
   the screen goes dark *and* the lamps go out. Its reset pin is wired to
   the P4's enable line, so a software reset of the P4 does *not* reset it;
   the driver rewrites every register at init.
2. **The pod has no interrupt line.** The firmware has to read the buttons
   by asking the MCP23017 200 times a second ("polling").
3. **SPI2 is shared by the IMU and the SD card.** While the SD card is busy
   writing (which can take hundreds of milliseconds), the IMU cannot be
   read. The IMU's FIFO buffers samples meanwhile.
4. **The panel bus is slow relative to the screen.** A whole 480×320 frame
   in RGB565 (2 bytes per pixel) is 307 kB and takes about 12 ms at the
   25 MHz write clock. So the firmware never redraws the whole screen if it
   can avoid it (Part 9).
5. **A flash write freezes core 0.** While the P4 erases or writes its own
   flash (saving settings, installing an update), ESP-IDF suspends the
   other core. The shift lamps are driven from core 0. So nothing writes
   flash while the bike is moving.

## 1.4 The two boards

There are two hardware targets, both P4-based:

- **The bike board** — the custom PCB above. Built from the repository root.
- **The demo board** — a Guition JC4880P443C_I_W, an off-the-shelf P4 board
  with a 4.3" touchscreen, used before the bike board existed and still
  used to try things away from the bike. Built from `demo_p4/`. It reuses
  `core`, `ui`, and several real services by source (`demo_p4/README.md`).

The P4 chips differ in silicon revision: the bike's is v3.2, the demo's
v1.3. An image built for one will not run on the other.

## 1.5 The repository

```
main/                  app_main, wiring (where every object is created), the USB console
components/bsp/        pins.hpp and buses.cpp: the only hardware truth
components/dash_hal/   abstract interfaces (IImu, IGnss, IPanel ...). Namespace `hal`.
components/drivers/    one class per chip, implementing a dash_hal interface
components/core/       pure logic, one directory per module. No ESP headers. Builds on a PC.
components/services/   the 13 FreeRTOS tasks that glue drivers to core
components/ui/         canvas, fonts, 23 widgets, ~30 screens, the App
components/doom/       Doom, as a "program" DIAG can launch; doomgeneric/ is the game
components/p4_restart_fix/  a workaround for a P4 v1 boot bug
test/                  host unit tests (plain CMake + ctest)
sim/                   the desktop simulator
tools/                 Python and C++ helpers (log analysis, flashing, fonts)
demo_p4/               the demo board's project, and c6_cp/ (the C6's firmware)
docs/                  documentation
releases/              every firmware image ever built
```

ESP-IDF's unit of code is the **component**: a directory with a
`CMakeLists.txt` that calls `idf_component_register(SRCS ... INCLUDE_DIRS ...
REQUIRES ...)`. `REQUIRES` lists the other components whose headers this one
may include. A component's public headers live in `include/<name>/`, so you
write `#include "core/lap/lap.hpp"`.

## 1.6 Building

Three separate builds, from one tree:

**Host tests** — `components/core/` and `components/ui/` compiled with your
PC's compiler, plus `test/test_*.cpp`. No ESP-IDF needed. Seconds to build.

```bash
cmake -S test -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

**Simulator** — the same `core/` and `ui/` plus `services/src/logic_tick.cpp`,
driven by a fake bike, drawing into a window (Python/Tk front end over a C++
process):

```bash
cmake -S sim -B build-sim && cmake --build build-sim
python3 sim/dash_sim.py
```

**Firmware** — needs ESP-IDF v6.0.2 activated in the shell:

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`idf.py build` produces `build/dash.bin`, and a post-build step copies it to
`releases/dash-<version>-<date>-<board>-<feature>.bin`. The version and the
feature word come from `firmware.txt`; bump it before a build you mean to
keep.

**sdkconfig.** ESP-IDF's build options (hundreds of `CONFIG_*` symbols)
live in a generated file, `sdkconfig`, which is per machine and not in git.
`sdkconfig.defaults` (in git) only *seeds* a new `sdkconfig`; editing the
defaults does nothing to an existing one. Because several options are
load-bearing (watchdog panic, NimBLE timers, partition offset, rollback),
`main/main.cpp` contains `#if !CONFIG_... #error "..."` guards that stop
the build and name the setting. The fix is to delete `sdkconfig` and
rebuild.

**Stack frames.** Our components compile with
`-Wframe-larger-than=1024 -Werror` (`cmake/dash_stack.cmake`): any function
whose stack frame exceeds 1 kB fails the build. Task stacks are 3–8 kB, and
a large local that is fine in a host test overflows a task on the board.
Big state goes in a `static`, a member, or PSRAM. A function that needs a
big frame on purpose is wrapped in `DASH_BIG_FRAME_BEGIN` / `END`
(`core/stack.hpp`) with a comment saying why.

---

# Part 2 — The rules, and the runtime

## 2.1 The five rules

These are enforced by review, by the build and by tests. Everything else in
the design follows from them.

1. **No globals.** A module gets data from its arguments. (v1 had a
   `globals.h` with 60 `extern`s. That is what v2 fixed.) Long-lived objects
   are created in exactly one place, `main/wiring.cpp`, and handed out by
   reference through a struct called `Deps`.
2. **One writer per piece of state.** Each piece of shared data has exactly
   one task that writes it. Other tasks read a consistent copy without a
   lock (Part 4). No mutexes for sensor data.
3. **`components/core/` never includes an ESP or FreeRTOS header.** Time is
   passed in as a parameter (`uint32_t now_ms`), never read from a clock.
   That is what lets all the logic run as unit tests on a PC.
   `test/check_no_esp_headers.sh` enforces it.
4. **A screen is a pure function of one `Frame`.** Screens never read
   sensors, never touch a bus, never call `get()` on shared state. They are
   handed a `Frame` (a snapshot of everything) and set widget values.
5. **A driver reports health; it does not decide policy.** A driver never
   calls `abort()`, never blocks forever, and never retries in a tight
   loop. It returns a `hal::Status` and lets someone else decide.

## 2.2 FreeRTOS in five minutes

- A **task** is a thread: a function `void f(void* arg)` with its own stack,
  created by `xTaskCreatePinnedToCore(f, "name", stack_bytes, arg,
  priority, &handle, core)`. Every task here is pinned to a core and never
  returns (its body is `for (;;) { ... }`).
- **Priority** is an integer; higher runs first. On a given core, the
  highest-priority task that is *ready* (not blocked) always runs, and
  preempts anything lower the moment it becomes ready. There is no
  time-slicing between different priorities.
- A task **blocks** by waiting on something: `vTaskDelay(ticks)`,
  `vTaskDelayUntil(&last, period)` (a fixed-rate loop), a queue receive, or
  `ulTaskNotifyTake()` (a lightweight per-task semaphore another task or an
  interrupt can "give" with `xTaskNotifyGive` / `vTaskNotifyGiveFromISR`).
- The **tick** is 1 ms here (`CONFIG_FREERTOS_HZ=1000`); `pdMS_TO_TICKS(ms)`
  converts.
- An **ISR** (interrupt service routine) runs outside any task, on a
  hardware event (a GPIO edge). It must be very short and may only call
  `...FromISR` functions.
- The **task watchdog** is a timer that panics the chip if a subscribed task
  does not "feed" it within 5 s. `ctrl`, `imu`, `logic` and `render`
  subscribe (`services::watchdog_join()` / `watchdog_feed()`), so a hung
  loop reboots the dash instead of freezing it. On a reboot, a **core dump**
  (the crashed task's stack and registers) is written to a flash partition;
  `keeper` reads it at the next boot and DIAG → System shows which task
  crashed.

## 2.3 The two cores, and why they are split

The P4 has two cores. The firmware treats them very differently:

**Core 0 is a real-time island.** Only these tasks run there:

| Task | Priority | Rate | Job |
|---|---|---|---|
| `ctrl` | 18 | 500 Hz + every tach edge | rpm, wheel speed, shift lamps, every LED-driver write |
| `imu` | 15 | 200 Hz (IMU interrupt) | read the IMU, lean/pitch/jumps |
| `gnss` | 12 | when bytes arrive | read GPS bytes, parse NMEA |
| `logic` | 10 | 50 Hz + on demand | lap timer, trip, fusion, commands, blackbox rows |
| `input` | 9 | 200 Hz | poll the pod, debounce |

No radio code, no filesystem, no flash writes. The reason is one number:
**a tach edge must reach the shift lamps in under 3 ms.** A rider shifting
at redline reacts to the lamps; late lamps are worse than none. `ctrl` is
the highest of our priorities on core 0, so nothing of ours can delay it.
Anything added to core 0 above priority 18 breaks that guarantee. ESP-IDF's
own `esp_timer` task sits at 22 on core 0, which is why NimBLE is configured
not to use `esp_timer` (`CONFIG_BT_NIMBLE_USE_ESP_TIMER=n`). ESP-Hosted
creates its SDIO tasks at priority 23 with no core affinity, so
`services::demote_radio_tasks()` finds them by name and lowers them to 8,
once before `ctrl` starts and again once a second.

**Core 1 takes everything that can be late:**

| Task | Priority | Rate | Job |
|---|---|---|---|
| `power` | 22 | 1 kHz | watch the 12 V supply; react to key-off |
| `render` | 6 | 120 Hz | draw the screen |
| `ble` | 4 | events | Bluetooth: phone, iPhone, watch |
| `keeper` | 3 | 20 Hz (health) / 1 Hz | health states, retries, saving to NVS, backlight level |
| `storage` | 3 | 2 Hz | SD mount/eject, saved-track file I/O |
| `wifi` | 3 | 4 Hz | the update hotspot and its web server |
| `logger` | 2 | drains | blackbox and detail logs to the SD card |
| `ota` | 2 | events | firmware slots and installs |

Plus ESP-IDF's and NimBLE's own tasks, a small console task
(`serial_btn`, priority 3, core 1) and, while it runs, Doom (priority 1).
`power` is high because it has a millisecond to act when the supply drops;
it does almost nothing the rest of the time.

The authoritative values are the `xTaskCreatePinnedToCore` call at the
bottom of each `components/services/src/*_service.cpp`.

## 2.4 Memory

The P4 has two kinds of RAM:

- **Internal SRAM** (~768 kB total, much of it used by ESP-IDF, the radio
  link and stacks). Fast. Task stacks and DMA buffers live here.
- **PSRAM** (32 MB). Slower (behind a cache), plentiful. Big buffers live
  here: map traces, log rings, the screen's shadow copy, the delta
  reference, Doom's arrays.

To put a static in PSRAM, annotate it `EXT_RAM_BSS_ATTR` (an ESP-IDF
macro; see `main/wiring.cpp`). There is a hard trap: **before the scheduler
starts, the only internal heap is a small region**, and ordinary statics
(`.bss`) eat into it. Too many and ESP-IDF cannot create its idle tasks and
the board boot-loops. After adding statics, check
`nm build/dash.elf | grep _heap_start` (features §28 has the history).

**No dynamic allocation after boot.** No `new` in a loop, no `std::vector`
growing, no `std::string`. Every buffer has a fixed size known at compile
time. No exceptions, no RTTI.

---

# Part 3 — How FreeRTOS is used

Part 2.2 gave the vocabulary. This part is the map: every FreeRTOS
primitive the firmware uses, where, and the reason it was chosen over the
alternatives. The rules of thumb that fall out of it are at the end
(3.12).

One structural fact first. **FreeRTOS appears only in `components/services/`,
`components/drivers/`, `components/doom/`, `components/bsp/` and `main/`.**
`components/core/` and `components/ui/` never include a FreeRTOS header
(rule 3). So every FreeRTOS object is either created inside a service and
kept private to it, or passed through `Deps` as an opaque `void*` (look at
`Deps::ota_stream`, `Deps::logic_task`, `CommandQueue::q_`) so that
`services/services.hpp`, which the host-built simulator also includes, never
names a FreeRTOS type. When you see `void* q_ = nullptr; // QueueHandle_t`,
that is why.

## 3.1 Inventory

| Primitive | Used for | Where |
|---|---|---|
| `xTaskCreatePinnedToCore` | Every one of our 13 tasks, plus the console task | each `*_service.cpp`'s `start_*()`, `main/serial_buttons.cpp` |
| `xTaskCreatePinnedToCoreWithCaps` | Doom's task, with its stack in PSRAM | `components/doom/src/doom.cpp` |
| `vTaskDelayUntil` | Fixed-rate loops that must not drift | `input` (200 Hz), `keeper` (20 Hz), `power` (1 kHz); `ctrl` keeps its own `next` |
| `vTaskDelay` | "Wait a bit and try again" after a failure; bring-up pauses | `gnss`, `storage`, `logger`, `ota`, `wifi`, drivers' init sequences |
| Direct-to-task notifications (`xTaskNotifyGive`, `vTaskNotifyGiveFromISR`, `ulTaskNotifyTake`) | Waking a task early, from an ISR or another task | tach ISR → `ctrl`; IMU data-ready ISR → `imu`; `gnss` / command queue → `logic`; Doom resume |
| Queues (`xQueueCreate`, `xQueueSend`, `xQueueReceive`, `xQueueSendFromISR`) | Passing small fixed-size messages between tasks | the command queue (→ `logic`), the input-event queue (`input` → `render`), NimBLE's event queue (NimBLE host → `ble`) |
| Stream buffer (`xStreamBufferCreateStatic`, `Send`, `Receive`, `Reset`) | A byte pipe | a Wi-Fi firmware upload: web server → `ota` (`Deps::ota_stream`) |
| Mutex (`xSemaphoreCreateMutexStatic`, `Take`, `Give`) | The one lock around the SD card | `drivers/sdcard` |
| Binary semaphore (`xSemaphoreCreateBinaryStatic`, `GiveFromISR`, `Take` with timeout) | Waiting for one hardware event with a timeout | the panel's TE pulse during `Ili9488::init()` |
| Task handles by name (`xTaskGetHandle`, `uxTaskPriorityGet`, `vTaskPrioritySet`) | Lowering tasks someone else created | `services::demote_radio_tasks` (ESP-Hosted's SDIO/RPC tasks and IDF's `sys_evt`) |
| `uxTaskGetSystemState`, `pcTaskName` | Every task's stack high-water mark | `services/src/stack_watch.cpp`, from `keeper` |
| `xTaskGetTickCount` | The schedule's own clock (ticks) | `ctrl`, `logic`, `input`, `keeper`, `power` |
| `vTaskDelete(nullptr)` | A task that has nothing left to do ends itself | `render` with no panel buffer; `ble` when the radio never comes up |
| `vTaskSuspend(nullptr)` | A task that must never run again but cannot be deleted safely | Doom after a fatal error (`DG_Exit`) |
| ESP-IDF task watchdog (`esp_task_wdt_add`, `esp_task_wdt_reset`) | Panic and reboot on a hung loop | `services/src/watchdog.cpp`, used by `ctrl`, `imu`, `logic`, `render` |
| FreeRTOS software timers | Not used by our code; NimBLE uses them (its "callouts") | `CONFIG_BT_NIMBLE_USE_ESP_TIMER=n` |

Not used, deliberately: **mutexes for data** (rule 2: `Published<T>`
instead), **event groups**, **critical sections** around shared state in our
own code (the ISR counters use atomics instead; see 3.4), and
**`malloc`-backed creation after boot** (every object above is created once,
at start-up, and most with the `...Static` variant so its memory is a
static rather than a heap allocation).

## 3.2 Creating tasks: the `start_*` pattern

Each service file ends with a function like this one, from
`components/services/src/imu_service.cpp`:

```cpp
void start_imu(Deps& d) {
    xTaskCreatePinnedToCore(imu_task, "imu", 4096, &d, 15, nullptr, 0);
}
```

Arguments, in order: the task function, its name (what stack reports and
the debugger show; `demote_radio_tasks` finds other people's tasks by this),
its stack in **bytes** (ESP-IDF's FreeRTOS port takes bytes, not words),
the `void*` argument, the priority, an optional out-handle, and the core.

Things to notice:

- **The argument is `&d`, the one `Deps`**, which lives in a static in
  `main/wiring.cpp` and outlives everything. The task casts it back:
  `Deps& d = *static_cast<Deps*>(arg);`. A task that needs two things
  (`logic` needs `Deps` and the `CommandQueue`) gets a static
  `std::pair<Deps*, CommandQueue*>` written once before the task exists
  (`g_logic_arg` in `logic_service.cpp`). Never pass the address of a local
  in `start_*`: it is gone when `start_*` returns, and the task reads garbage.
- **Every task is pinned.** No task floats between cores, so the core-0
  budget can be reasoned about by listing what is pinned there.
- **Stack sizes are fixed by hand**, typically 4096. `render`'s is
  `8192 + sizeof(ui::App)` because the `App` (and its one live screen)
  lives on render's stack. `logger` has 6144, `logic` 5120, `input` and
  the console 3072. `services::watch_stacks` (from `keeper`, once a second)
  walks `uxTaskGetSystemState` and puts the task with the least headroom on
  DIAG → System, and logs every task's at 10 s, so a stack that is too
  small shows up before it overflows. A stack overflow on this chip is a
  crash, often far from the cause.
- **Order matters** and lives in `app_main` (`main/main.cpp`): `ctrl`
  first (lamps live before the panel has woken), then `render`, then the
  sensors' tasks, then core-1 housekeeping, `ble` and `wifi` last so a
  radio that will not start delays nothing (Part 6).
- **Doom is the one task created with its stack in PSRAM**
  (`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`),
  created lazily the first time DIAG → Doom is opened, at priority 1 on
  core 1 so it only ever gets what every other core-1 task leaves.

## 3.3 Loop shapes

Every task body is `for (;;) { ... }`. The interesting part is *how it
waits*, because that decides its timing. There are four shapes in this
codebase.

**(a) Fixed rate, no drift: `vTaskDelayUntil`.** `input`, `keeper` and
`power`:

```cpp
TickType_t next = xTaskGetTickCount();
for (;;) {
    vTaskDelayUntil(&next, kPeriod);   // sleeps until next, then next += kPeriod
    ...work...
}
```

`vTaskDelayUntil` measures from the *previous wake time*, not from now, so
work time does not accumulate into drift. This matters for `input`: the
debounce in `core::input` assumes reads are one period apart. After an
overrun (a dead pod costs a 10 ms I2C timeout, two periods), `input` resets
`next` to now rather than doing a burst of catch-up reads, because a burst
of reads milliseconds apart would confuse the debounce:

```cpp
vTaskDelayUntil(&next, kPeriod);
if (xTaskGetTickCount() - next > kPeriod) next = xTaskGetTickCount();
```

**(b) Fixed rate *plus* early wake: a deadline and a notification.**
`logic` and `ctrl`. Their schedule is periodic, but some events must be
handled *now*, not at the next tick. They sleep on `ulTaskNotifyTake` with a
timeout equal to the time left until the next deadline. A notification ends
the sleep early; the timeout ending it means the deadline arrived.
`logic_service.cpp`, lightly trimmed:

```cpp
constexpr TickType_t kPeriod = pdMS_TO_TICKS(20);    // 50 Hz
TickType_t next = xTaskGetTickCount();
for (;;) {
    watchdog_feed();
    const TickType_t left = next - xTaskGetTickCount();
    if (static_cast<int32_t>(left) > 0) ulTaskNotifyTake(pdTRUE, left);
    const TickType_t now_ticks = xTaskGetTickCount();
    const bool due = static_cast<int32_t>(now_ticks - next) >= 0;
    if (due) {
        ulTaskNotifyTake(pdTRUE, 0);         // wakes that came with the tick are its
        next += kPeriod;
        if (static_cast<int32_t>(now_ticks - next) >= 0) next = now_ticks + kPeriod;
    }
    logic_tick(d, g_state, commands, d.clock->now_ms(),
               due ? LogicPass::Scheduled : LogicPass::Event);
}
```

Read it carefully, because it is the pattern to copy:

- `ulTaskNotifyTake(pdTRUE, timeout)` returns the notification count and
  clears it (`pdTRUE`). Several gives before the take collapse into one
  wake. That is what you want: "there is news", not "there are 7 news".
- An early wake runs an **Event pass** and does *not* move `next`, so the
  50 Hz grid that blackbox rows depend on stays even no matter how many
  fixes and commands arrive.
- A wake that lands on or after the deadline counts as the tick, so a busy
  producer cannot starve the schedule.
- `static_cast<int32_t>(a - b) >= 0` is the wrap-safe "a is at or after b"
  comparison for unsigned tick counts. You will see it everywhere a time is
  compared. Never write `a >= b` for two times that can wrap.
- After an overrun (a flash write froze core 0 for 20 ms), `next` is reset
  to now plus a period instead of running the missed ticks back to back.

`ctrl` is the same idea at 500 Hz with the tach ISR as the early waker:
each spark edge wakes it, it recomputes the lamp pattern from the edge's
timestamp and writes the LED driver at once, then goes back to waiting for
the rest of its 2 ms tick.

**(c) Event-driven, blocking on a source.** `gnss` blocks inside the UART
driver's read (`IGnss::read(..., wait_ms)`) and wakes when bytes arrive;
`ble` blocks in `xQueueReceive` on its event queue; `ota` waits on its
request slot and the stream buffer; `imu` blocks in `ulTaskNotifyTake` until
the IMU's data-ready interrupt gives it, with a short timeout as a fallback:

```cpp
if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFallbackMs)) == 0 && ...) {
    // no edge arrived: count it (SystemInfo::imu_timeouts) and read anyway
}
```

The timeout is slightly longer than the 5 ms sample period, so a working
interrupt always wins and a broken one costs one sample of latency. That
fallback is "the whole safety net for the INT1 register values", and DIAG
→ System counts how often it fires: climbing at 200 a second means the
interrupt is not configured right and the task has quietly fallen back to
polling.

**(d) Deadline with an interruptible wait: `render`.** `render` paces itself
to a 120 Hz deadline computed from a fixed start time in milliseconds (so
the 8.33 ms period does not drift by rounding to 1 ms ticks), and waits on
the *input-event queue* with a timeout of the time left:

```cpp
if (left > 0 && wait_input_event(ev, left)) {     // xQueueReceive with timeout
    app.on_input(ev, canvas);
    while (take_input_event(ev)) app.on_input(ev, canvas);   // drain the rest
}
```

A button press therefore ends the wait and is drawn at once; with no press
the wait times out on the deadline and a frame is drawn. The pacing bounds
how stale a *number* on screen can be (8.3 ms), not how fast a menu reacts.

## 3.4 Interrupts and task notifications

Three ISRs matter.

**The tach and wheel edges** (`drivers/src/engine_sense.cpp`). A GPIO
interrupt fires on every edge of the tach, front-wheel and rear-wheel
inputs. The ISR is `IRAM_ATTR` (it runs from internal RAM, so it works even
while flash is busy), and does three things: rejects an edge inside the
channel's refractory window (spark ringing produces several edges per
spark), increments a counter, and stores the edge's timestamp
(`esp_timer_get_time()`, microseconds). For the tach channel only, it then
wakes `ctrl`:

```cpp
if (TaskHandle_t t = g_tach_waiter.load(std::memory_order_relaxed)) {
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(t, &woke);
    if (woke) portYIELD_FROM_ISR();
}
```

`woke` is set if the notified task has a higher priority than the one the
ISR interrupted; `portYIELD_FROM_ISR()` then switches to it as the ISR
returns, instead of at the next tick. That is the difference between the
lamps reacting in microseconds and in up to a millisecond.

`ctrl` registered itself as the waiter with
`drivers::EngineSense::notify_on_tach(xTaskGetCurrentTaskHandle())` at the
top of its task. The counters and timestamps are plain variables written
only by the ISR; `IEngineSense::read()` latches them all together so the
count and the timestamp agree (reading them separately at 500 Hz produced an
rpm spike every few seconds). The ISR's channel objects are the one place in
the driver layer where file-scope state is allowed, because a GPIO ISR takes
a `void*` and nothing else; the comment above `edge_isr` defends it.

**The IMU's data-ready line** (INT1 → GPIO13), set up in
`imu_service.cpp`. The ISR is one `vTaskNotifyGiveFromISR` to the `imu`
task's handle, captured before the task first blocks.

**The panel's TE (tearing-effect) pulse**, only during `Ili9488::init()`.
The panel pulses TE once per refresh if it has been initialised correctly,
and the driver uses that to tell which of two panel wirings is fitted: a
**binary semaphore** created with `xSemaphoreCreateBinaryStatic`, given from
the TE ISR, taken with a timeout. A timeout means "no TE, this guess was
wrong". The ISR handler is removed straight after.

Why a task notification for the first two and a semaphore for the third?
A notification is lighter (no separate object, it lives in the task's
control block) but targets one specific task; the TE wait happens inside a
driver call on whichever task is running init, and a semaphore keeps the
driver from needing to know which. Both are "binary-ish": several gives
before a take count once.

## 3.5 Queues

Three queues exist, all created once, all of fixed-size items, all used
**without ever blocking the sender**.

**The command queue** (`services::CommandQueue`, `services/src/queues.cpp`).
Depth 16, items are `core::state::Command` (an id and a small union).
Producers: `render` (screens post through `ui::ICommandSink`), the Wi-Fi
page's server task (settings edits), `imu` (its calibration result as two
`ConfigSet`s). Consumer: `logic` only.

```cpp
bool CommandQueue::post(const core::state::Command& c) {
    if (xQueueSend(q, &c, 0) != pdTRUE) { ++dropped_; return false; }   // timeout 0
    if (void* t = consumer_.load(std::memory_order_relaxed))
        xTaskNotifyGive(static_cast<TaskHandle_t>(t));                  // wake logic now
    return true;
}
```

Two decisions to understand. **Timeout 0**: a screen posting into a full
queue gets `false` and the drop is counted; it never waits. A render task
that stalled because `logic` was busy would be a dashboard that stops
drawing, which is the wrong way round. **The notify after the send**: the
queue alone would let `logic` find the command at its next 50 Hz tick, up to
20 ms later. The notification turns that into an Event pass now. `logic`
drains the queue with `take()` (receive, timeout 0) in a loop at the top of
every pass. There is also `post_from_isr()` for completeness, using
`xQueueSendFromISR` and `vTaskNotifyGiveFromISR` and the same
`portYIELD_FROM_ISR` dance.

`logic_tick` itself never sees a FreeRTOS queue: it takes an
`ICommandSource&`, a one-method interface. The firmware adapts the queue to
it (`QueueSource` in `logic_service.cpp`); the simulator passes a
`std::deque`. That is how the simulator runs the real decision code.

**The input-event queue** (also in `queues.cpp`). Depth 16, items are
`core::nav::InputEvent` (button + short/long). Producer: `input` (and the
console task, through `post_input_event`). Consumer: `render`, which waits
on it (3.3 (d)). It is deliberately separate from the command queue: "the
rider pressed a button" is UI navigation and belongs to `render`; "something
asked for a state change" belongs to `logic`. Merging them would put menu
navigation on `logic`'s critical path and on core 0.

**NimBLE's event queue** (`ble_service.cpp`). NimBLE runs its own host task
and calls our callbacks (connected, disconnected, a Feed write arrived, a
heart-rate notification) *on that task*. Those callbacks do no work: each
copies what happened into an `Event` struct and `xQueueSend(g_ctx.q, &e, 0)`,
counting a drop if the queue is full. The `ble` task blocks in
`xQueueReceive` and does the work: parses lines with `core::media`, decides
pairing with `core::ble`, publishes slots. That keeps "one writer per slot"
literal: only the `ble` task ever publishes `media`, `notify`, `heart` and
`ble_diag`, never NimBLE's task. (NimBLE's sync and reset callbacks take no
argument, so the queue is reached through one file-scope object, `g_ctx`,
the single acknowledged exception to rule 1 in that component.)

## 3.6 The stream buffer: a firmware upload

A Wi-Fi upload is megabytes arriving over HTTP on ESP-IDF's web-server task,
but the only task allowed to write the dev firmware slot is `ota`. Between
them sits a FreeRTOS **stream buffer**, a byte FIFO with one writer and one
reader, created statically in `ota_service.cpp` (16 kB) and handed over as
`Deps::ota_stream` (a `void*`).

- The `wifi` service first publishes `OtaUpload{size}` (a slot) so `ota`
  knows an upload of that many bytes is coming, then the server's handler
  `xStreamBufferSend`s each chunk as it arrives, waiting up to 10 s for room
  (a send that times out fails the HTTP request with a 500).
- `ota` `xStreamBufferReceive`s with a timeout and writes the flash a sector
  at a time, with exactly the checks it applies to an install from the SD
  card (header, board, parked-only, final hash).
- If nothing arrives for 10 s the upload is abandoned and the slot left
  invalid; `xStreamBufferReset` clears the pipe for the next attempt.

A stream buffer suits this better than a queue because the data is a byte
stream with no natural message boundary, and better than a shared buffer and
a flag because it provides the back-pressure for free: a full buffer makes
the HTTP handler wait, which slows the upload to the speed flash can be
written.

## 3.7 The one mutex: the SD card

Four tasks touch the SD card: `logger` (writes logs), `storage` (mount,
eject, track files), `ota` (reads update images), `wifi` (serves and deletes
logs, receives game files). The FAT filesystem library is not safe for
concurrent use from several tasks mid-operation, so `drivers::SdCard` holds
**one mutex** (`xSemaphoreCreateMutexStatic`), and every public method takes
it through an RAII guard:

```cpp
class Hold {
public:
    explicit Hold(SemaphoreHandle_t l)
        : l_(l), held_(xSemaphoreTake(l_, kLockWait) == pdTRUE) {}   // kLockWait = 3 s
    ~Hold() { if (held_) xSemaphoreGive(l_); }
    bool held() const { return held_; }
    ...
};
```

Note the **timeout**: a caller that cannot get the card within 3 s gets
`hal::Status::Busy`, not a hang (rule 5). The logger treats Busy as "try the
block again later", not "card dead". This is a mutex rather than a binary
semaphore because FreeRTOS mutexes have **priority inheritance**: if
`storage` (priority 1) holds the card and the logger (priority 2) wants it,
`storage` is temporarily raised so it finishes and releases sooner. The
other way round does not work: a higher-priority task that releases and
retakes the lock in a loop (a route's ~1,100 block reads) never lets a
lower one in, which is why `storage` sits under the logger (2.6).

The mutex protects the *card*, which is a device. It is not a data lock.
Sensor data is never behind a mutex (rule 2).

## 3.8 Demoting someone else's tasks

ESP-Hosted creates its SDIO and RPC tasks inside its own component at
priority 23 with no core affinity, and offers no configuration for either.
Left alone they could run on core 0 above `ctrl`. So `ble_service.cpp` has a
list of their names and lowers each one it finds:

```cpp
constexpr const char* kHostedTasks[] = {
    "sdio_rx_buf", "sdio_read", "sdio_process_rx", "sdio_write",
    "rpc_rx", "rpc_tx", "rpc_supp_cb", "sys_evt",
};
for (const char* name : kHostedTasks)
    if (TaskHandle_t t = xTaskGetHandle(name))
        if (uxTaskPriorityGet(t) > kHostedPrio) vTaskPrioritySet(t, kHostedPrio);   // 8
```

`main` calls it before `ctrl` exists (the SDIO tasks are already created by
ESP-Hosted's static constructor). `ble` calls it again once the link is up
and periodically from its loop, and `wifi` when the hotspot starts, because
some of these tasks (the RPC ones, `sys_evt`) only exist later. Priority 8 is below every core-0 task
of ours and above `render`, so the radio still beats the panel on core 1.
It logs how many it found: **if an ESP-Hosted upgrade renames its tasks, this
finds zero and they stay at 23.** The boot log line `esp_hosted tasks: N
found` is the check.

## 3.9 The task watchdog

`services/src/watchdog.cpp` wraps ESP-IDF's task watchdog:

```cpp
void watchdog_join() { (void)esp_task_wdt_add(nullptr); }   // subscribe the calling task
void watchdog_feed() { (void)esp_task_wdt_reset(); }
```

`ctrl`, `imu`, `logic` and `render` call `watchdog_join()` once before their
loop and `watchdog_feed()` at the top of every iteration. If one of them
fails to feed for `CONFIG_ESP_TASK_WDT_TIMEOUT_S` (5 s), the watchdog fires.
`CONFIG_ESP_TASK_WDT_PANIC` makes that a panic: the chip writes a core dump
to flash and reboots, and `main.cpp` refuses to build without that setting.
Consequences you must design for:

- **Nothing in those four loops may block for seconds.** Every wait in them
  has a timeout well under 5 s. A driver call that can hang (an I2C read on
  a wedged bus) must have its own short timeout.
- The core dump write erases up to ~116 kB of flash before the reboot, so a
  crash costs more dark-dash time than the 5 s alone. bringup §2 measures it.
- Tasks not in the list (`keeper`, `logger`, ...) are not watched. They are
  allowed to wait on slow things (the card), which is exactly why they are
  on core 1 and not subscribed.

## 3.10 How time is read

Two clocks, never mixed up:

- **FreeRTOS ticks** (`xTaskGetTickCount()`, 1 ms each). Used only for a
  loop's own schedule (`next`, `vTaskDelayUntil`). It stops advancing while
  the scheduler is suspended, for example during a flash write.
- **Milliseconds since boot from `esp_timer`**, through `hal::IClock`
  (`Deps::clock->now_ms()`, `now_us()`). This is the time handed to `core/`
  as `now_ms` and stamped into slots (`Gnss::rx_ms`, `Frame::t_ms`). It is a
  hardware counter and keeps counting through a suspended scheduler.

`core/` never reads either (rule 3); it is always handed `now_ms`. A task
reads `d.clock->now_ms()` once per pass and passes the same value to
everything it calls in that pass, so one pass sees one instant.

## 3.11 What core 1 looks like at run time

Putting priorities together, from highest to lowest on core 1:
`power` (22) → ESP-Hosted's tasks after demotion (8) → `render` (6) →
NimBLE host (IDF-created) and `ble` (4) → `keeper`, `storage`, `wifi`, the
console (3) → `logger`, `ota` (2) → Doom (1) → the FreeRTOS timer task
(1, running NimBLE's callouts) → idle.

Two consequences:

- **`logger` is at 2 on purpose.** ESP-IDF's SD-over-SPI driver waits out a
  card's busy time by *spinning*, not sleeping. Whatever priority `logger`
  had, everything below it starved for as long as the card was busy (tens
  to hundreds of ms). At 5 it starved `keeper`'s 20 Hz health table. At 2 it
  starves only `ota` and Doom.
- **`render` can be preempted** by `power` and the radio. At 120 Hz a frame
  that slips is invisible. That is the trade core 1 makes.

## 3.12 Rules of thumb

1. Never block a sender. Queue sends use timeout 0 and count drops.
2. Every wait has a timeout, and every timeout is under the watchdog's 5 s
   if the task is watched.
3. Wake a task with a notification when it must act *now*; let it find the
   data in a slot. The notification carries no data (3.5, Part 4).
4. ISRs are `IRAM_ATTR`, do the minimum, use `...FromISR` calls and
   `portYIELD_FROM_ISR`.
5. Create every FreeRTOS object once, at boot, preferably `...Static`.
6. FreeRTOS types never appear in a header that `core/`, `ui/`, the sim or
   the host tests include. Pass handles as `void*`.
7. Compare times wrap-safely: `static_cast<int32_t>(a - b) >= 0`.
8. A new task: pick its core by whether it can be late (if it can, core 1),
   its priority by what it may preempt, and list it in `services.hpp`'s
   table and in `CLAUDE.md`.

---

# Part 4 — Shared state: slots and the Frame

This is the most important part of the guide. Every number the rider sees
travels the same road: a producer task computes it, **publishes** it into a
**slot**, and the render task reads every slot once per frame into a
**`Frame`**, which screens are handed. Files:

- `components/core/include/core/state/published.hpp` — `Published<T>`
- `components/core/include/core/state/vehicle.hpp` — every payload struct
- `components/core/include/core/state/slots.hpp` — the `Slots` table and the `Frame`
- `components/core/include/core/state/track_buffer.hpp` — `TrackBuffer<T,N>`, for bulk data

## 4.1 `Published<T>`: one writer, many lock-free readers

```cpp
template <typename T, typename Hook = NoPublishHook>
class Published {
public:
    void publish(const T& v);         // the owning task only
    T    get() const;                 // any task, any core
    uint32_t generation() const;      // how many publishes so far
private:
    std::atomic<uint32_t> buf_[2][kWords];   // two copies of T, as 32-bit words
    std::atomic<uint32_t> count_;
};
```

How it works:

- There are **two buffers**. `count_` says how many publishes have
  completed; buffer `count_ & 1` holds the latest.
- `publish(v)` writes `v` into the *other* buffer, `(n + 1) & 1`, word by
  word with relaxed atomic stores, then a release fence, then
  `count_.store(n + 1, release)`. Until that last store, readers keep reading
  the old buffer, which nobody is touching.
- `get()` loads `count_` (acquire), copies buffer `n & 1` word by word, then
  checks `count_` again. If it changed during the copy (the writer
  published twice, so it may have started overwriting the buffer being
  read), it retries. Otherwise the copy is consistent.
- `generation()` returns `count_`. A reader keeps the last value it saw and
  compares: "has anything new arrived since I looked?" without copying the
  payload. `ctrl` does this with the health table (it only re-reads it when
  `keeper` has published a new one), and every request slot works this way
  (5.4).

Why not a mutex? A mutex would let a low-priority reader on core 1 hold up
`ctrl` on core 0. Why not a seqlock (the original design)? A seqlock reader
that preempts its own writer *on the same core* spins forever, because the
writer cannot run to finish its write: `ctrl` at priority 18 reading the
`gnss` slot while `gnss` (priority 12) was half way through publishing it
did exactly that. With two buffers, a reader always has a complete one to
read.

**Requirements on `T`:** trivially copyable (`static_assert`ed): no
`std::string`, no pointers into memory a reader might outlive, no virtual
functions. Fixed-size `char` arrays for text. Every struct in `vehicle.hpp`
obeys this.

**The one-writer rule is a contract, not a mechanism.** Nothing stops two
tasks calling `publish` on the same slot; it would corrupt it. The comment
beside each slot in `slots.hpp` names its owner, and that is the law.

**Cost.** A publish or a get copies the whole struct, through 32-bit atomics.
That is cheap for 40 bytes and fine for a few hundred, and the reason bulk
data (thousands of map points) is not in a slot (4.6).

## 4.2 The slot table

`core::state::Slots` is one struct of `Published<...>` members. It is
created once in `main/wiring.cpp` (`g_slots`) and every task reaches it as
`d.slots`.

| Slot | Payload | Writer | Read by |
|---|---|---|---|
| `engine` | `Engine` | `ctrl` | `logic`, `ota`, `power`, and `still_for_flash` (for `keeper`, `ble`); the Frame |
| `motion` | `Motion` | `ctrl` | `logic`, `imu`, `ota`, `wifi`, `still_for_flash`; the Frame |
| `attitude` | `Attitude` | `imu` | `logic`, `logger` (detail log), `keeper` (the USB IMU stream); the Frame |
| `gnss` | `Gnss` | `gnss` | `ctrl`, `imu`, `logic`, `keeper`, `logger`, `still_for_flash`; the Frame |
| `trip` | `Trip` | `logic` | `keeper` (saves odometer), `power` (saves at key-off), `logger`; the Frame |
| `lap` | `Lap` | `logic` | the Frame (and `logic` itself, for the detail log) |
| `elec` | `Elec` | `keeper` | `ctrl` (USB vs bike), `logic`, `logger`; the Frame |
| `battery` | `Battery` | `power` | `keeper`, `logic`, `logger`; the Frame |
| `media` | `Media` | `ble` | the Frame |
| `tpms` | `Tpms` | `ble` (once a TPMS scan exists; never written today) | the Frame |
| `health` | `Health` | `keeper` | `ctrl`, `logic`, `logger`, `wifi`; the Frame |
| `ble_diag` | `BleDiag` | `ble` | `logger`; the Frame |
| `waypoint` | `Waypoints` | `logic` | `keeper` (saves to NVS); the Frame |
| `fused` | `Fused` | `logic` | `ctrl` (speedo source), `imu` (lean's speed); the Frame |
| `gear_cal` | `GearCal` | `logic` | the Frame |
| `session` | `Session` | `logic` | the Frame |
| `perf` | `Perf` | `logic` | the Frame |
| `stunt` | `Stunt` | `logic` | the Frame |
| `notify` | `Notify` | `ble` | the Frame |
| `heart` | `Heart` | `ble` | `logic` (recovery, the blackbox `HR` column); the Frame |
| `hr_recovery` | `HrRecovery` | `logic` | the Frame |
| `ota` | `Ota` | `ota` | `wifi`; the Frame |
| `wifi` | `Wifi` | `wifi` | `logger`; the Frame |
| `media_ctrl` | `MediaCtrl` | `logic` | `ble` — **not in the Frame** |
| `ble_request` | `BleRequest` | `logic` | `ble` — not in the Frame |
| `ota_request` | `OtaRequest` | `logic` | `ota` — not in the Frame |
| `imu_request` | `ImuRequest` | `logic` | `imu` — not in the Frame |
| `storage_request` | `StorageRequest` | `logic` | `storage` — not in the Frame |
| `wifi_request` | `WifiRequest` | `logic` | `wifi` — not in the Frame |
| `ota_upload` | `OtaUpload` | `wifi` | `ota` — not in the Frame |

The last seven are **hand-offs**, not display data (5.4).

## 4.3 The Frame and `snapshot()`

```cpp
struct Frame {
    Engine engine; Motion motion; Attitude att; Gnss gnss; Trip trip; Lap lap;
    Elec elec; Battery battery; Media media; Tpms tpms; Health health;
    BleDiag ble_diag; Ota ota; Wifi wifi; Waypoints waypoint; Fused fused;
    GearCal gear_cal; Session session; Perf perf; Stunt stunt; Notify notify;
    Heart heart; HrRecovery hr_recovery;
    uint32_t t_ms = 0;
};
Frame snapshot(const Slots& s, uint32_t t_ms);
```

`snapshot` calls `get()` on each display slot in turn and stamps `t_ms`
(the render task's `now`). Each member is internally consistent (it came
from one publish), but **the Frame as a whole is not one instant**: the
lean angle may be 2 ms old and the lap time 20 ms old. That is fine for a
display and stated as such in `slots.hpp`.

Who builds Frames:

- `render`, once per frame (`core::state::snapshot(*d.slots, now)`), and
  passes it to `ui::App::update()`.
- The simulator, the same way.
- `test_ui`, which builds Frames as literals (`Frame f; f.engine.rpm =
  6000; ...`) and checks what screens draw. This is rule 4's payoff.

**Nobody writes a Frame back.** A Frame is a value. Screens get
`const Frame&`. The only thing that ever modifies one is `ui::App` itself,
in one place: the start-up gauge sweep rewrites a *copy* (`swept = f;
core::sweep::apply(swept, ...)`) so the needles run to full scale and back,
and hands the copy to the screen. No slot is touched.

**Conventions every field follows:**

- **Units in names**: `_mph`, `_ft`, `_deg`, `_g`, `_ms`, `_s`, `_c`, `_v`,
  `_pct`, `_bpm`. All internal values are imperial (mph, ft, mi, gal);
  conversion to metric happens only at display time (`core/units`).
- **Zero usually means "none yet" / "unknown"**, and the struct says so
  per field. Where 0 is a legitimate value and "unknown" must be different,
  there is a separate flag (`Lap::delta_valid`, `Fused::speed_valid`,
  `Gnss::fix`, `Heart::link`).
- **`seq` fields** count events (a new notification, a finished perf run, a
  new trick). The UI keeps the last `seq` it acted on and shows a toast
  when it moves. That is how a one-off event crosses a state-based
  interface without being lost or repeated.
- **`*_at_ms` / `*_done_ms` fields** are system-clock times of events, so a
  screen can show "for 5 s after X" without keeping history (a Frame has no
  memory).
- Health decides whether to show a number: a widget bound to a device that
  is not `ok()` shows `--`, never a stale value or a zero.

## 4.4 Every field

### `Engine` — written by `ctrl` at 500 Hz

| Field | Meaning | How it is produced |
|---|---|---|
| `rpm` | Engine speed for display | `core::tach::Meter::rpm()`: edge intervals from the tach ISR, smoothed by CONFIG → Shift → Tach smooth (`tach_smooth_ms`, a time constant) |
| `neutral` | In neutral (sensed) | `IEngineSense::read().neutral`, which the driver has already inverted from the LOW-active wire |
| `tps_pct` | Throttle position | From the ECU through the K-line adapter (features §35); 0 without one. `ecu_adapter`, `engine_c`, `intake_c`, `map_kpa` and `inj_raw` come the same way |
| `ecu_online` | ECU link up | `d.engine->online()`, always false today; screens show `--` for ECU fields |
| `shift_mask` | Which of the 8 lamps the rpm lights, bit 0 = lamp 1 | `core::shift::pattern()` on the *raw* rpm (or the lead-predicted rpm), with hysteresis against the previous mask. Published so screens mirror the real lamps rather than recomputing (they would have their own hysteresis state and disagree) |
| `shift_flash` | Over the flash limit (lamps blinking) | `core::shift::over_limit()` |
| `rpm_peak` | The rpm reached before the last shift, held ~0.5 s | `core::shift::PeakHold` (CONFIG → Shift → Peak hold); 0 for none |
| `shift_hold` | The one lamp that keeps the held peak lit | `core::shift::hold_lamp()`, masked so it never duplicates a lamp the rpm lights |
| `gear` | Inferred gear, 1..6, 0 = unknown | `core::gear::estimate(rpm, best_mph, neutral, gear table)`: nearest rpm/mph ratio. 0 when the clutch is in, stopped, or nothing matches |

### `Motion` — written by `ctrl` at 500 Hz

| Field | Meaning | How |
|---|---|---|
| `front_mph`, `rear_mph` | Each wheel's speed | `core::tach::WheelMeter` on that wheel's pulses, circumference and magnets per rev (CONFIG → Wheels), smoothed by `wheel_smooth_ms` |
| `gps_mph` | GPS Doppler speed, copied from the `gnss` slot | — |
| `best_mph` | **The speed everything uses**: speedo, odometer, gear, gear calibration, trip, session | `core::fusion::shown_speed_mph(...)`: the front wheel, unless fusion rejected it on its last pass or it is far from the fused speed (a wheelie, a locked front, a dying sensor); then the fused speed; then the rear; then GPS. A wheel faulted by `WheelCheck` (health table) is skipped |
| `accel_g` | Reserved; not filled today | — |

### `Attitude` — written by `imu` after each FIFO drain (~200 Hz)

| Field | Meaning |
|---|---|
| `lean_deg` | Lean angle, + right. From `core::attitude::Filter`: gyro integration corrected towards `atan(v·Ω/g)` (Ω the world yaw rate), with the bar-mount steering angle removed (Part 8) |
| `pitch_deg` | + nose up |
| `yaw_rate_dps` | World yaw rate |
| `g_lat`, `g_lon`, `g_vert` | Specific force in the bike frame, low-passed (~10 Hz). `g_lon` feeds the fusion filter's prediction |
| `airborne`, `airtime_ms`, `last_land_g`, `land_ms` | From `core::jump::Detector`: in the air now; the last jump's duration and landing peak; when it landed (0 = no jump yet). A screen shows a jump for a few seconds after `land_ms` |
| `imu_temp_c` | The IMU die temperature, from its FIFO packets; 0 until read |
| `steer_deg`, `accel_lean_deg`, `heading_deg`, `slide_rate_dps`, `sliding`, `bias_g*_dps` | The filter's insides, for the detail log only. No screen reads them |

### `Gnss` — written by `gnss` on every sentence it parses

| Field | Meaning |
|---|---|
| `fix` | Position is valid. Goes false when the receiver says so, or 2.5 s after the last new fix (`core::nmea::age_fix`) |
| `sats`, `quality`, `hdop` | Satellites in use, fix quality, horizontal dilution of precision (lower is better; fusion only lets a fix with HDOP ≤ 2.5 referee the wheels) |
| `lat`, `lon` (double), `alt_m` | Position, altitude above sea level |
| `course_deg` | Direction of travel, from the receiver (meaningless when stopped) |
| `speed_mph` | Doppler speed, from the receiver, not differenced positions |
| `utc_ms` | Time of day, UTC, in ms — **time of day only** |
| `day_of_year`, `year`, `month`, `day` | From the RMC date, 0 until a sentence carried one. `core/sun` needs the season; the logger names files by date |
| `rx_ms` | System clock when this fix's first sentence finished arriving. Moves once per fix, so `logic` compares it to tell a new fix from the same one read again |

### `Trip` — written by `logic` every pass

`odo_mi`, `trip_a_mi`, `trip_b_mi` (distance integrated from `best_mph`),
`fuel_used_gal` (`core::trip::FuelModel`: burn interpolated between idle and
wide-open rates by load: an *estimate*), `eng_hours` (time with rpm > 0),
`top_mph`. Odometer and hours are restored from NVS at boot
(`logic_restore`) and saved by `keeper` while still and by `power` at key-off.

### `Lap` — written by `logic` every pass

| Field | Meaning |
|---|---|
| `phase` | `core::lap::Phase`: no line, armed (waiting for the first crossing), timing |
| `count`, `current_ms`, `last_ms`, `best_ms` | Laps completed, the running lap, the last, the session best |
| `delta_ms`, `delta_valid` | Gain/loss against the reference lap (− = ahead). `delta_valid` false when there is no reference or the rider is off its line: "0" and "no idea" are different statements |
| `dist_to_line_ft` | Distance to the finish line |
| `sector`, `split_sector`, `split_ms`, `split_delta_ms`, `split_delta_valid`, `split_at_ms`, `ideal_ms` | Sectors (`core::lap::Sectors`): which is being ridden (−1 none); the last split crossed, its time and delta against that sector's best, when; the ideal lap (sum of best sectors, 0 until all three exist) |
| `last_done_ms`, `last_was_best` | When the last lap ended and whether it was a session best (LAP flashes LAST for 5 s) |
| `max_lean_left_deg`, `max_lean_right_deg` | The session's deepest lean each way above 10 mph. Kept here, not in a widget, because a screen is destroyed when left |
| `track`, `track_recalled` | The saved track (0..15) this line belongs to, −1 none; whether it came back by itself on approach |
| `line_refused_seq` | Moves each time a Set line was refused for want of speed (the line's direction is the GPS course, meaningless at a standstill); the App shows a toast |
| `track_best_ms`, `track_ideal_ms` | That track's record and ideal over all visits |

### `Session`, `Perf`, `Stunt`, `HrRecovery` — written by `logic`

- **`Session`** (`core::session`): `dist_mi`, `moving_s`, `engine_s`,
  `top_mph`, `avg_mph` (over moving time), `best_air_ms`, `hardest_land_g`,
  `jumps`. For TRACK → SESSION.
- **`Perf`** (`core::perf`): `phase` (0 idle, 1 armed: stopped and ready,
  2 launching, 3 braking), `live_ms`/`live_ft` of the run under way,
  `metric` (which thresholds the runs used), `last[7]`/`best[7]` results
  indexed by `Perf::Metric` (0–30, 0–60, 60 ft, 330 ft, 1/8 mile, 30–0,
  60–0; `ms`, and `x` = trap mph or braking ft), and `seq`/`event`/
  `event_best` for the toast.
- **`Stunt`** (`core::stunt`): the trick in progress (`live_kind`,
  `live_ms`, `live_ft`, `live_deg`, `live_points`, `live_balance`), the
  combo (`combo`, `combo_x10` = next multiplier ×10), the last trick
  (`seq`, `kind`, `grade`, `landing`, `ms`, `ft`, `peak_deg`, `points`,
  `mult_x10`, `record`, `done_ms`), and session totals.
- **`HrRecovery`** (`core::hr::Recovery`): `measuring`, `start_ms`,
  `start_bpm`, `live_drop`, the last result (`drop1`, `drop2`, `from_bpm`,
  `has_drop2`), `best_drop1`, `count`, `seq`.

### `Elec` and `Battery`

- **`Elec::vbat`** — written by `keeper` (1 Hz): VIN through the divider
  plus CONFIG → Battery → Trim, i.e. battery volts. 0 = no reading, which
  `core::power::classify` reads as "unknown supply" (`ctrl` then falls back
  to "engine turning means bike power" when capping the lamps on USB).
- **`Battery`** — written by `power` (on change, and once a second):
  `rest_v` (read once per key-on, after the fuel pump's prime), `crank_min_v`
  (the starter's dip), `charge_v`, `verdict` (`core::battery::Verdict`:
  good, weak, not charging, overcharging). 0 = not measured yet.

### `Media`, `Notify`, `Heart`, `BleDiag` — written by `ble`

- **`Media`**: `title`, `album`, `artist`, `lyric` (60-byte UTF-8 each,
  bounded and cut on a character boundary by `core::media`), `state`
  (0 stop, 1 play, 2 pause), `src` (which path filled it: phone app or an
  iPhone's AMS), `updated_ms` (0 = nothing has ever arrived; screens treat
  that as "no media"), and the playback clock as an **anchor**:
  `position_ms` was true at dash time `position_at_ms`; a screen shows
  `position_ms + (f.t_ms - position_at_ms)` while playing. `duration_ms` 0 =
  unknown. After the phone disconnects, `ble` keeps the slot 10 s then
  publishes it zeroed.
- **`Notify`**: the phone's last notification: `kind` ('C' call, 'M'
  message, 'X' missed call, 'O' other), `ringing`, `who`, `text`, `at_ms`,
  `seq`. One, not a list: the Frame is copied every frame.
- **`Heart`**: `link` (Off, Searching, Connecting, Live), `bpm` (0 when the
  last reading had no pulse in it), `at_ms`, `worn`, contact flags,
  `rssi_dbm`, `name`, session `avg/max/min_bpm`, `readings`, `seq`,
  `zone_max_bpm` (CONFIG's Max HR, carried so HEART can draw zones from the
  Frame alone). `core::hr::fresh(h, now)` decides whether `bpm` is still
  worth showing.
- **`BleDiag`**: DIAG → Radio's data: RSSI, the last Feed tag accepted,
  Ctrl subscribed, `apple` bits (iPhone service discovery/AMS/ANCS live),
  `encrypted`, `bonds`, `pairing_s` left in the pairing window, `c6_fw`
  ("2.12.8 #1a2b3c4d").

### `Ota`, `Wifi`

- **`Ota`** (written by `ota`): `known` (has published once), `role`
  (stable or dev, from the bootloader's rollback state), the running
  image's version/slot/date, what the other slot holds, and an install in
  progress (`install`, `install_pct`, `error`, `image_error`). A dev image
  shows a red DEV tag on every screen because the App watches `role`.
- **`Wifi`** (written by `wifi`): `state`, this session's `pass` (8 digits,
  shown on DIAG → Update), `clients`, `off_in_s` (the idle timer).

### `Waypoints`, `Fused`, `GearCal`

- **`Waypoints`** (`logic`): five `Waypoint`s in `core::waypoint` order
  (START, TRUCK, CAMP, FUEL, MARK), each `set`, `lat`, `lon`, `utc_ms`,
  `day`. START is set by `logic` at the first good fix of a ride if it is
  stale; the rider drops and clears the others from NAV.
- **`Fused`** (`logic`): `speed_mph`, `speed_sigma_mph` (1σ), `speed_valid`
  (false after 2 s with no correction), `used`/`rejected`/`suspect` (bit per
  source, `core::fusion::Source`), `resets`, `accel_bias_g`.
- **`GearCal`** (`logic`): the guided calibration's phase, the gear being
  measured, steady time vs needed, live ratio and error, results per gear,
  the wheel-circumference check against GPS, and the background learner's
  evidence per gear. `ui/src/screen_gearcal.cpp` replaces CONFIG while `phase`
  is not 0.

### `Health` — written by `keeper` (20 Hz, on change)

An array of `DevHealth`, indexed by `core::state::Dev`:

```
Imu, Gnss, LedDriver, McpPod, Sd, Ecu, Display, BleHost, Phone,
TpmsF, TpmsR, FrontWheel, RearWheel
```

Each `DevHealth` has `state` (`Absent`, `Init`, `Ok`, `Degraded`, `Fault`),
`limited` (working but short of spec, e.g. GPS stuck at 1 Hz, which makes it
Degraded while its data is still good), `fault_count`, `last_ok_ms`,
`last_err` (the driver's own code, shown verbatim on DIAG). Two helpers:
`h.ok(dev)` (exactly Ok) and `h.working(dev)` (Ok, or Degraded only for
being limited). Part 5.6 explains how states are decided.

### `Tpms`

Front and rear `TpmsWheel` (`psi`, `temp_c`, `batt_pct`, `alarm`, `age_ms`,
`valid`). **Never written today**: there is no TPMS scan. Screens show it
blank. It exists so adding the scan changes no screen.

### `Frame::t_ms`

The render task's clock when the snapshot was taken. Screens use it for
anything relative ("5 s since the lap ended", the media position), never a
clock of their own.

## 4.5 The hand-off slots (not in the Frame)

`MediaCtrl`, `BleRequest`, `OtaRequest`, `ImuRequest`, `StorageRequest`,
`WifiRequest`, `OtaUpload`. Each is a tiny struct (`uint8_t op` or `cmd`).
They carry a request from `logic` (or `wifi`) to the one task that can act
on it. They have no sequence number: **`generation()` is the sequence
number.** The receiver keeps the generation it last handled:

```cpp
// in the receiving task's loop
const uint32_t gen = d.slots->ota_request.generation();
if (gen != seen_gen) {
    seen_gen = gen;
    const core::state::OtaRequest r = d.slots->ota_request.get();
    ...act on r.op...
}
```

Why not post the command straight to that task's own queue? Because then
`logic` would not be the one place every command passes through, and every
command would need a new queue. With this pattern, everything the rider asks
for goes through one queue, one switch statement (`logic_tick`), and one log
line in the detail log's `_evt` file.

## 4.6 Bulk data: `TrackBuffer` and friends

Large, append-only data is never in a slot (copying 64 kB per publish would
be absurd). It lives in purpose-built single-writer structures, created in
`wiring.cpp` (mostly in PSRAM) and handed out through `Deps`:

| Object | Type | Writer | Reader |
|---|---|---|---|
| `trip_trace` | `TrackBuffer<TrackPoint, 4096>` | `logic` (one point per fix, ≤10 Hz) | CRUISE → MAP via `IPointSource` |
| `lap_trace` | same | `logic` (cleared each lap) | TRACK → MAP |
| `lap_history` | `services::LapHistory` (64 laps + peaks) | `logic` | TRACK → LAPS, through `App::set_lap_history` |
| `hr_history` | `core::hr::History` | `ui::App` (render) | STAT → HEART's graph |
| `delta` | `core::delta::Tracker` (two point traces) | `logic` | `logic` (computes `Lap::delta_ms`) |
| `blackbox` | `core::log::Ring<Record, 512>` | `logic` | `logger` |
| `imu_log`, `tach_log`, `det_log`, `evt_log` | detail-log rings | `imu`, `ctrl`, `logic`, `logic` | `logger` |
| `tracks` | `services::TrackMailbox` | `logic` / `storage`, alternating | the other |

`TrackBuffer<T, N>` is a ring of `N + 1` slots and a `std::atomic<size_t>
write_` counting every push ever made:

- `push(v)` (writer only) writes `buf_[w % (N+1)]`, a release fence, then
  bumps `write_`.
- `read(index, out)` (any reader) refuses an index not yet written or already
  overwritten, copies, then re-checks `write_`: if the writer lapped the
  reader during the copy, it returns false rather than a torn point.
- The spare slot (`N + 1`) means the writer is never writing the slot holding
  the oldest readable point while a reader copies it.
- `read_nth(i, out)` reads the i-th of the currently readable points, oldest
  first: `for (i = 0; i < size(); ++i)` is how a map draws.

The **`LapHistory` publishing order** is a small lesson in lock-free code
(`logic_tick.cpp`): `logic` fills the new row's peaks and time *first* and
increments `count` *last*, so the render task on the other core never sees a
counted row that is half written.

---

# Part 5 — The wiring: `Deps`, commands, settings, health

## 5.1 `services::Deps`: the replacement for globals

`components/services/include/services/services.hpp` defines one struct,
`Deps`, holding a pointer or a reference to every long-lived object a task
might need. `main/wiring.cpp` creates every object as a file-scope static
(the only place in the firmware allowed to), fills one `Deps g_deps`, and
`app_main` passes `g_deps` by reference to every `start_*()`.

**Hardware interfaces** (abstract `hal::` types; the concrete driver is
known only to `wiring.cpp`):

| Member | Interface | Concrete (bike) | Used by |
|---|---|---|---|
| `clock` | `hal::IClock` | `drivers::EspClock` (esp_timer) | everyone |
| `imu` | `hal::IImu` | `drivers::Icm42688p` | `imu` |
| `gnss` | `hal::IGnss` | `drivers::Pa1616d` | `gnss` |
| `pod` | `hal::IButtonPod` | `drivers::ButtonPod` (MCP23017) | `input` |
| `panel` | `hal::IPanel` | `drivers::Ili9488` | `render` |
| `sense` | `hal::IEngineSense` | `drivers::EngineSense` | `ctrl` |
| `lamps` | `hal::IShiftLamps` | `drivers::Pca9955b` | `ctrl` only |
| `engine` | `hal::IEngine` | `services::EspNowEngine` (offline without the adapter) | `ctrl`; `wifi` calls `attach()` |
| `storage` | `hal::IStorage` (NVS) | `drivers::NvsStorage` | `wiring` (load settings, panel model), `logic_restore`, `keeper`, `power`. NimBLE keeps its bonds in NVS through its own store |
| `card` | `hal::ICard` (SD) | `drivers::SdCard` | `logger`, `storage`, `ota`, `wifi`, Doom |
| `rails` | `hal::IRailSense` | `drivers::VinSense` | `keeper`, `power` |

**State and shared objects:**

| Member | What | Notes |
|---|---|---|
| `slots` | `core::state::Slots*` | Part 4 |
| `settings` | `core::config::Settings*` | one writer at run time: `logic` (5.5) |
| `health` | `core::health::Tracker*` | everyone reports; `keeper` derives (5.6) |
| `system` | `ui::SystemInfo*` | platform numbers for DIAG → System (heap, stacks, counters); filled field by field, each field with one writer: `keeper` (heap, uptime, temperature, stacks, crash, pod counters), `ctrl` (`ctrl_late`), `imu` (timeouts, catch-up), `gnss` (sentence counts), `logger` (rows, drops), `render` (`render_hz_x10`, `first_frame_ms`), `storage` (`sd_ejected`). Plain fields, not atomics: each is one aligned word with one writer, read only for display |
| `trip_trace`, `lap_trace`, `hr_history`, `lap_history`, `delta`, `tracks` | bulk data | 4.6 |
| `blackbox`, `imu_log`, `tach_log`, `det_log`, `evt_log` | log rings | Part 7 (`logger`) |
| `ota_stream` | `void*` → FreeRTOS `StreamBufferHandle_t` | 3.6 |
| `commands` | `ui::ICommandSink*` | the command queue, for tasks not handed it at start (the Wi-Fi page) |
| `games`, `program` | flag, `ui::IProgram*` | DIAG → Doom |
| `logic_task` | `std::atomic<void*>` → `TaskHandle_t` | for `wake_logic()` |

**Atomics carried in `Deps`**: small flags that are *signals between tasks*
rather than vehicle state, each with one writer:

| Member | Writer | Readers | Meaning |
|---|---|---|---|
| `power_fail` (`atomic<bool>`) | `power` | `ctrl`, `logger`, `keeper` | VIN just dropped: lamps dark, no new SD writes, backlight off |
| `log_flush_req` / `log_flush_ack` (`atomic<uint32_t>`) | `storage` / `logger` | the other | Eject handshake: storage bumps req, logger flushes its RAM blocks and copies req into ack |
| `retry_requests` (`atomic<uint32_t>`, bit per `Dev`) | `keeper` sets bits (`request_retry`) | the owning task clears its bit (`take_retry`) | "re-initialise this device" (5.6) |
| `backlight_pct` (`atomic<uint8_t>`) | `keeper` (`render` once, at the first frame) | `ctrl` | the backlight level to send |
| `buttons_held` (`atomic<uint8_t>`) | `input` | Doom | raw held buttons, before debounce |

Helper functions in the same header: `wake_logic(d)`, `request_retry(d,
dev)`, `take_retry(d, dev)`, and `still_for_flash(d)`, which builds a
`core::flash::Stillness` from the `motion`, `gnss` and `engine` slots and
answers "may a task write flash right now?" (`keeper` and the BLE bond store
ask it before every flash write).

## 5.2 The command queue

UI-originated changes go *down* as `core::state::Command`s
(`core/state/command.hpp`): a `CommandId` and a union argument (`f`, `i`,
or `cfg{key, val}`). Screens post through `ui::ICommandSink` (the `post()`
helper in `ui::Screen`), and `logic` is the only consumer (3.5 has the
queue mechanics). The rule: **a screen never changes state, it asks.**
`logic` decides whether the ask is valid. Setting a finish line, for
example: the screen only has a Frame, so it cannot know whether the GPS fix
is good enough; `logic` checks `core::lap::may_set_line(fix, speed)` and
refuses otherwise.

Every command, who posts it, and what `logic_tick` does with it:

| Command | Posted by | `logic` does |
|---|---|---|
| `LapSetLine` | RACE/LAP long E (`screens_track.cpp`), CONFIG → Lap → Set line here | if moving with a fix: sets the line through the bike perpendicular to its course, forgets splits and the reference, starts saving it as a track; else bumps `Lap::line_refused_seq` |
| `LapArm` | LAP short E | re-arms: the next crossing starts a lap |
| `LapClear` | CONFIG → Lap → Clear laps | drops the line, laps, history, trace, delta; no track recall until next power-on |
| `TrackForget` | CONFIG → Lap → Forget track | blanks the saved track record, queues its file for deletion, then does everything `LapClear` does |
| `TripReset` (`i`: 0 = A, 1 = B) | TRIP long E; CONFIG → Trip | resets that trip meter |
| `ConfigSet` (`cfg.key`, `cfg.val`) | CONFIG screen, Wi-Fi page, `imu` (calibration result) | `core::config::set()` through the table's clamp; a gear entry also becomes the learner's base; refreshes the fuel model; resets the log decimator |
| `MediaCtrl` (`i`: 0 prev, 1 play/pause, 2 next) | SPEED's C/D/E (`screens_cruise.cpp`) | publishes `media_ctrl` for `ble` |
| `BleForgetPhone`, `BlePairingMode` | CONFIG → Phone | publishes `ble_request` |
| `WifiToggle` | CONFIG → Wi-Fi, DIAG → Update | publishes `wifi_request` |
| `NavDropWaypoint` / `NavClearWaypoint` (`i`: which) | NAV short / long E | with a fix, stores / clears that waypoint and publishes `waypoint` |
| `ImuCalibrate` | CONFIG → IMU → Calibrate level, STAT → IMU | publishes `imu_request`; `imu` answers later with two `ConfigSet`s for the mount offset |
| `StorageEject` | CONFIG → SD card → Eject | publishes `storage_request` |
| `OtaInstallSd`, `OtaBootDevOnce`, `OtaPromote` | DIAG → Update (`screens_diag.cpp`) | publishes `ota_request` |
| `GearCalStart` | CONFIG → Gearing → Calibrate gears | opens the guided run (`core::gearcal::Guide`) |
| `GearCalNext` / `GearCalSkip` / `GearCalCancel` | the calibration page's E / long E / B | advance / skip a gear / close; accepting writes the table and its base |
| `GearCalReset` | CONFIG → Gearing → Reset to base | puts the table back to its calibrated base |
| `LeanPeaksReset` | STAT → IMU short E | zeroes the session lean peaks |
| `PerfReset` | STAT → PERF E | clears the performance bests |
| `StuntReset` | STAT → STUNT E | new stunt session |
| `LogStart`, `LogStop`, `StorageToHost`, `ImuSetMountOffset` | **nobody** | **nothing**: declared, never posted, fall through `default:`. Reserved ids (USB mass storage would use `StorageToHost`) |

When the detail log is on, every command is written to `_evt` as it is
acted on (`log_command`), with the new value for a `ConfigSet`.

## 5.3 Why `logic` is the hub

Look at what passes through `logic_tick` (`services/src/logic_tick.cpp`)
every pass: every command, every fix, the speed filter, the lap timer, the
trip computer, the stunt scorer, gear calibration, the blackbox row. Two
reasons it is one task:

1. **One writer for the most-shared things.** Settings, laps, trips,
   waypoints and the fused speed each need exactly one writer; putting them
   in one task means they can be updated together, consistently, without
   any locks between them.
2. **One function the simulator can call.** `logic_tick()` has no FreeRTOS
   in it. The firmware calls it from a 50 Hz loop; the simulator calls it
   from its own loop with a fake bike. Same decisions, same bugs.

## 5.4 The request pattern, end to end

Take DIAG → Update → Install from SD:

1. `UpdateScreen::handle()` (render, core 1) posts
   `Command(CommandId::OtaInstallSd)`.
2. `CommandQueue::post` sends it and notifies `logic`.
3. `logic` (core 0) wakes, drains the queue, and publishes
   `OtaRequest{InstallSd}` to `slots->ota_request`.
4. `ota` (core 1) sees `ota_request.generation()` move, reads the request,
   checks `core::ota::may_write` (parked: not moving, engine off), and
   either starts the install or records the refusal.
5. `ota` publishes progress in `slots->ota` (`install`, `install_pct`,
   `error`), which reaches the Update screen through the next Frame.

No task called another. Every step is a publish or a queue. Every step has
one writer.

## 5.5 Settings

`core::config::Settings` (`core/config/config.hpp`) is a plain struct with
every tunable in the firmware: brightness, units, wheel circumferences,
shift-light thresholds, lap gates, gear ratios, fuel rates, log rate, IMU
mount offsets, and so on. It is trivially copyable. One instance,
`g_settings`, lives in `wiring.cpp`; tasks get `Deps::settings`.

**The ConfigItem table** (`core/src/config.cpp`) describes every user-facing
setting once:

```cpp
struct ConfigItem {
    uint16_t key;          // permanent NVS key; never renumbered, never reused
    const char* label;     // "Front circ."
    const char* group;     // "Wheels": CONFIG's submenu
    Kind kind;             // Float, Int, Bool, Enum, Action
    float min, max, step;
    const char* unit;
    uint16_t offset;       // byte offset into Settings, from offsetof
    uint8_t width;         // size of that member, taken from the member itself
    const char* const* options; uint8_t option_count;   // Enum
    state::CommandId action;                            // Action
};
```

Entries are built with helpers (`num(...)`, `boolean(...)`, `choice(...)`,
action items) and a `FIELD(member)` macro that supplies `offset` and
`width`, for example:

```cpp
num    (0x0B02, "Sweep time", "Startup", Kind::Int, 500, 5000, 100, "ms", FIELD(sweep_ms)),
boolean(0x0B03, "Splash",     "Startup", FIELD(splash)),
```

An offset rather than a pointer, because a pointer table would have to
point into one specific global `Settings`, which rule 1 forbids. The same
table drives: the CONFIG screen (rendered from it, grouped by `group`), the
NVS load and save (`services/src/settings_store.cpp`), the Wi-Fi page's
settings section, the detail log's settings dump, and `validate_table()`
(checked at boot and by `test_config`). Adding a setting is one member in
`Settings` and one line in the table (Part 13).

Everything crosses the interface as a `float` (`get`, `set`, `clamp`,
`nudged`), including bools and enums, so there is one clamp for everyone.
Modules that keep their own config structs get them through views:
`shift_view()`, `lap_view()`, `gear_view()`. Those are called each pass, so
nothing keeps using a stale threshold.

**The write path, and why it is this way:**

1. The CONFIG screen holds a `const Settings*` (`ui::Context::settings`).
   It can read but not write. When the rider presses D on "Front circ.", it
   computes `nudged(item, current, +1)` and posts
   `Command::config_set(key, value)`.
2. `logic` applies it with `config::set()` (the one writer). The Wi-Fi page
   and the IMU calibration post the same command, so all three paths share
   one clamp and one writer.
3. Tasks that copy a setting into their own state compare against
   `Settings` once a pass and re-read on a difference (`imu` does this with
   the mount offsets).
4. `keeper`, once a second, **only when `still_for_flash()`**, runs
   `SettingsSaver::save_changed()`: it diffs `Settings` against a private
   copy of what was last written and writes only the changed keys to NVS
   namespace `cfg`, then commits.

A setting changed at speed therefore takes effect at once but is saved the
first second the bike is still. Boot reads them back through
`load_settings()` before anything else is set up (Part 6), and a key missing
from NVS keeps its default: that is what a fresh board and a newly added
setting both look like.

## 5.6 Health: reporting, deciding, retrying

`core::health::Tracker` (`core/health/health.hpp`, `core/src/health.cpp`)
holds one `Cell` per `Dev`, made of atomics, split by who may write what:

- **Facts, written by any task that talks to the device**:
  `report_ok(dev, now)` after a successful transaction (stamps
  `last_ok_ms`, bumps `ok_gen`, clears the error streak);
  `report_error(dev, err, now)` after a failed one (bumps `streak` and
  `err_total`, keeps `last_err`); `report_limited(dev, bool)`.
- **Intent, written at init**: `begin_init(dev, now)` (a device must call
  this before it reports; a report for an absent device is ignored),
  `set_absent(dev)` (not fitted, or ejected: an ejected SD card stays
  ejected however late a last write reports on it).
- **Derived, written only by `keeper`**: `tick()` turns facts into a
  `DevState`; `claim_retry()` decides when to re-init;
  `publish_if_changed()` publishes the `health` slot.

`tick()` per device, with the default `Policy` (degrade after 3
consecutive errors, fault after 10, stale after 2 s quiet, 10 s grace while
initialising, retry every 5 s):

```
Absent  -- begin_init -->  Init
Init    -- any report_ok -->  Ok
Ok/Init -- 3 errors in a row, or quiet > 2 s (10 s while Init) -->  Degraded
Degraded-- 10 errors in a row, or quiet again / quiet > 4x window -->  Fault
any     -- report_ok -->  Ok          (except Fault: see retry)
Ok + limited  -->  shown as Degraded  (the data is still good: working() is true)
Fault   -- keeper's claim_retry, every 5 s -->  Init (grace re-stamped)
```

"Quiet" matters as much as errors: an MCP23017 that browned out answers
every read with "no buttons" and never errors. If nothing reports success
for two seconds, the device is not trusted.

**Retries are done by the owner, not by `keeper`.** When a device is in
Fault and its retry interval has passed, `keeper` calls
`request_retry(d, dev)`, which sets a bit in `Deps::retry_requests`. The
task that owns the device checks `take_retry(d, dev)` between its own
transactions and re-inits there:

| Device | Re-init runs on |
|---|---|
| `Imu` | `imu`, between FIFO reads |
| `Gnss` | `gnss` (changes the UART baud rate, so must not happen under its reads) |
| `McpPod` | `input` (recovery deletes the I2C driver) |
| `LedDriver` | `ctrl` (the only writer of I2C1) |
| `Display` | `render` (and repaints the whole screen afterwards) |

This rule exists because every one of those used to be done by `keeper`
from core 1, and every one landed in the middle of another task's
transaction: an IMU soft reset under a FIFO read, a baud change under the
NMEA parser, a panel reset mid-frame, an I2C driver deleted under a pod
poll.

`FrontWheel` and `RearWheel` are judged by `logic` (`core::fusion::
WheelCheck`: a wheel reading 0 for 5 s while the rest of the bike says it is
moving is Dead) and reported like any other device. `ctrl` reads the health
table, but only when its generation changes, and skips a faulted wheel when
it picks the speedo's source.

**What the rider sees:** every widget bound to a device asks
`frame.health.ok(dev)` (or `working`) and shows `--` otherwise.
`core::health::worst()` picks the single worst device for SPEED's one-line
fault readout; DIAG → Sensors shows the whole table; `core::alert` raises
an overlay for the faults that matter while riding (SD card stopped, lamps
gone, IMU gone, a dead wheel sensor).

---

# Part 6 — Boot, step by step

What happens between the key turning and the speed appearing. Source:
`main/main.cpp` (`app_main`) and `main/wiring.cpp` (`build`,
`finish_panel`, `finish_sensors`).

**Before our code.** The P4's ROM loads the second-stage bootloader from
flash offset 0x2000. The bootloader reads the partition table at 0x10000
and the `otadata` partition to choose which app slot to run (stable,
normally; Part 11), skips the image hash on a cold power-on
(`CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`, to save time; a software
restart still checks it), maps the app into the cache, and jumps to
ESP-IDF's start-up code. That code runs C++ static constructors (including
ESP-Hosted's, which creates its SDIO tasks and buffers), starts FreeRTOS,
and calls `app_main` on a task of its own.

**`app_main`**, in order:

1. Logs `dash v2 — N config items` and runs `core::config::validate_table()`.
   A malformed table is reported now rather than as a setting that silently
   will not save.
2. **`wiring::build()`**:
   1. `bsp::board_init()` brings up every bus at once: SPI2 (IMU + SD), I2C0
      (pod, 100 kHz), I2C1 (PCA9955B, 1 MHz), UART1 (GPS), the i80 LCD bus
      and the GPIO ISR service, and drives safe pin levels. A driver never
      creates a bus; it adds a device to one that exists.
   2. Constructs the panel driver with the LCD bus handle; attaches the LED
      driver and the pod to their I2C buses.
   3. **NVS and the settings first** (`g_storage.init()`,
      `services::load_settings()`), because the settings decide how
      everything else is configured.
   4. **What the shift lamps need, and nothing else**: the PCA9955B (comes up
      with every output dark), the engine-sense counters and ISRs, VIN
      sense. `ctrl` starts the moment `build()` returns, so the lamps are
      live before the panel has woken.
   5. Marks devices that are not fitted as **Absent** (ECU, TPMS) and the
      phone as absent until one connects. The GPS and the SD card are *not*
      brought up here: the GPS takes seconds to negotiate a baud rate and
      the card can take as long to mount, so `gnss` and `storage` do it on
      their own tasks while the dash is already running.
   6. Fills every `Deps` member, ticks the health tracker once, publishes
      the health slot, and creates the command and input queues.
3. `services::demote_radio_tasks(true)` lowers ESP-Hosted's already-created
   tasks below `ctrl`.
4. **`start_ctrl`**: lamps and rpm are live.
5. **`wiring::finish_panel()`**: reads the panel model fitted last boot
   (`hal::kKeyPanelModel` in NVS namespace `life`), tries that wiring first,
   inits the panel (`Ili9488::init()`, which tells the two supported panels
   apart by which wiring makes TE pulse), saves the model only if it
   changed (a flash write; this happens with `ctrl` already running, but
   only when a panel was swapped, which happens parked), and sets the
   backlight to 20 %.
6. **`start_render`**: the first frame goes out (the splash), timed into
   `SystemInfo::first_frame_ms`. The start-up sweep, on both the screen and
   the lamps, is timed from that moment so they move together.
7. **`wiring::finish_sensors()`**: IMU init (waits ~50 ms for its gyro) and
   pod init, while `render` draws.
8. `start_imu`, `start_gnss` (which now negotiates with the GPS),
   `start_logic` (whose first act is `logic_restore`: odometer, engine
   hours and waypoints back from NVS), `start_input`, and the console
   (`start_serial_buttons`).
9. Core-1 housekeeping: `start_logger`, `start_storage` (mounts the card),
   `start_keeper`, `start_power`, `start_ota` (works out whether this image
   is the stable or the dev one).
10. **Radio last**: `start_ble` (brings up ESP-Hosted's link to the C6,
    checks and if needed updates the C6's firmware, starts NimBLE) and
    `start_wifi` (idle until asked). A C6 that never answers delays nothing
    above it.
11. Logs `running` and returns; its task is deleted.

Every `bring_up(dev, status)` call records the outcome in the health table
and logs it, and **nothing fails the boot**: a dash that refuses to start
because the GPS is unplugged is worse than a dash with no GPS, and every
screen already shows `--` for a dead device.

**On screen**: a 1 s splash (name, firmware version, a progress bar), then
the gauge sweep (rpm, speed and gear to full scale and back, default 1 s,
aborted if the bike moves), then CRUISE → SPEED.

---

# Part 7 — Every task, in depth

Each subsection: what it owns (writes), what it reads, its loop, and the
traps. Files are `components/services/src/<name>_service.cpp` unless noted.

## 7.1 `ctrl` — core 0, priority 18, 500 Hz + every tach edge

**Owns**: the `engine` and `motion` slots, every write to the PCA9955B
(lamps, lamp brightness, backlight, config refresh, re-init), the `_tach`
detail-log ring, `SystemInfo::ctrl_late`.

**Reads**: `IEngineSense` counts, `Settings` (shift, wheels, tach), the
`gnss`, `fused`, `elec` and `health` slots, `Deps::backlight_pct`,
`power_fail`, `SystemInfo::first_frame_ms`.

**One iteration** (a 2 ms tick, or a tach edge waking it early):

1. Feed the watchdog. Latch the counters: `d.sense->read(c)` gives pulse
   totals and the timestamp of the last edge on each channel.
2. Update `core::tach::Meter` (rpm from edge *intervals*, not from counts
   per tick: at 6000 rpm pulses are 10 ms apart and four ticks in five see
   none) and the two `WheelMeter`s.
3. Update the peak-hold (the rpm before the last shift, and its one lamp).
4. **Decide the lamp mask first**, before publishing anything:
   `core::shift::lamp_rpm()` (the raw rpm, or where it will be `lead_ms`
   ahead if CONFIG → Shift → Lead time is set), the start-up sweep while it
   runs, `core::shift::pattern()` with hysteresis against the previous
   mask, the held lamp, the over-limit flasher. Dark while `power_fail`.
   The *raw* rpm drives the lamps; smoothing is for the number a human
   reads and would add lag here.
5. If `keeper` requested a LED-driver re-init (`take_retry`), do it here and
   mark everything to be resent.
6. **Write only what changed**: the mask if it differs from what the chip
   last acknowledged; the master brightness (capped on USB by
   `core::power::lamp_master_pct` when VIN says the supply is USB); the
   backlight from `backlight_pct`. Plus, spread over different ticks so no
   tick carries two, a once-a-second resend of the mask, the backlight, and
   the chip's configuration registers (a browned-out PCA9955B acknowledges
   writes with every current register at zero and looks alive). After a
   failed write, no more I2C this tick and none for 50 ms: a wedged bus
   would otherwise cost a timeout every tick (rule 5).
7. Re-read the health table **only if its generation moved**, to know
   whether a wheel sensor is faulted.
8. Build and publish `Motion` (with `best_mph` from
   `core::fusion::shown_speed_mph`) and `Engine` (with `gear` from
   `core::gear::estimate`).
9. Report the LED driver's health, but only on a tick that actually wrote
   (a quiet tick is evidence of nothing).
10. Every 10th tick (50 Hz), push a `_tach` detail row: pulse intervals,
    extra/missed pulses, rejected ringing, edge-to-wake and edge-to-lamp
    latency, this tick's work time.

**The rule for this file**: nothing in it may block, allocate, touch the
filesystem or log. If a line can wait, it belongs in `logic`.

## 7.2 `imu` — core 0, priority 15, ~200 Hz

**Owns**: the `attitude` slot, the `_imu` ring, the IMU's health, the
`SystemInfo` IMU counters. **Reads**: `IImu`, `motion`, `fused`, `gnss`,
`imu_request`, `Settings` (mount offsets, rake, wheelbase).

Waits on the data-ready interrupt (3.3 (c)); on wake, takes a `keeper`
retry if one is pending, then **drains the FIFO completely** in bounded
batches (after an SD stall several batches can be waiting, and reading one
per wake would fall further behind each time). Every sample goes through
`core::attitude::Filter` (lean, pitch, yaw rate, g vector) and
`core::jump::Detector`, using the fused speed for the `v·Ω` term. Each new
GPS fix is fed to the filter's slide check *after* the samples up to its
arrival, so its heading history covers when the fix was taken. At boot it
zeroes the gyro bias from a still second (by the same stillness test as
Calibrate level; a bike that never sits still keeps zero bias rather than
a wrong one). An `ImuRequest{CalibrateLevel}` makes it collect a still
second and post the resulting roll and pitch offsets as two `ConfigSet`s,
so `Settings` keeps its one writer; it then waits for `logic` to apply them
before comparing again. Every raw sample (sensor axes, before mount and
bias) is pushed to `_imu` for the detail log, so a ride can be re-filtered
on a laptop.

**Axis convention**: x forward, y left, z up, right-handed; satisfying it
is the driver's job (`icm42688p.cpp` maps chip axes to bike axes).
`core/attitude/attitude.hpp` has the full sign table. A sign error gives a
smooth, confident, wrong lean angle.

## 7.3 `gnss` — core 0, priority 12, on UART data

**Owns**: the `gnss` slot, GPS health, `SystemInfo` sentence counts.

First brings the receiver up (`Pa1616d::init()`: asks it to move from 9600
to 57600 baud and from 1 Hz to 10 Hz, and proves each step by hearing a
sentence back). The result is reported as Ok, **limited** (stuck at 1 Hz:
Degraded, data still used) or an error. Then loops: block in the UART read
until bytes arrive, feed them to `core::nmea` (the parser; RMC and GGA
sentences), and when a sentence completes, publish the updated `Gnss`. A new
UTC time means a new fix: stamp `rx_ms` and call `wake_logic(d)`, so the
lap timer sees a fix within ~1.5 ms of its sentence ending, not at the next
20 ms tick. A receiver that has gone silent has its fix aged out
(`core::nmea::age_fix`, 2.5 s) and is reported. Retries (`take_retry`) are
done here because they change the UART's baud rate.

## 7.4 `logic` — core 0, priority 10, 50 Hz + on demand

The task body is 3.3 (b). The work is `services::logic_tick(d, state,
commands, now, pass)` in `services/src/logic_tick.cpp`, with all its memory
in one `LogicState` (a static, far too big for a stack). One pass, in order:

1. **Read** `gnss`, `motion`, `engine` (and later `attitude`, `health`,
   `heart`). Build the lap timer's config from `Settings`; at 1 Hz GPS
   (health `limited`) widen its fix-gap gate.
2. **Commands**: drain the queue, act on each (5.2).
3. **Fused speed**: build a `core::fusion::SpeedInput` (IMU `g_lon`, both
   wheels, GPS Doppler and HDOP, whether this is a new fix), step the
   Kalman filter, publish `fused`. Then `WheelCheck` judges each wheel and
   reports `FrontWheel`/`RearWheel` health.
4. **Gear calibration** (Scheduled passes only, because its windows average
   samples and extra samples at each fix would bias them): the guided run if
   open, else the background learner if enabled. Publish `gear_cal`.
5. **START waypoint**: once per power-on, at the first good fix with a date,
   reset START if it is stale (unset, another day, or 6 h old).
6. **Saved tracks**: one turn of the mailbox with `storage` (`pump_tracks`);
   place a line set before the track library was loaded; recall a saved
   track if a good fix is within 1500 ft of one and no line is set.
7. **Trip**: `core::trip::Computer::update()` on `best_mph` and engine-on;
   publish `trip`.
8. **Session**, **performance timers** (every pass: thresholds are
   interpolated between passes, so more passes are finer timing), **heart
   rate recovery**, **stunts** (Scheduled only: points integrate over time).
   Publish each.
9. **Lap**: once per *new* fix (comparing `rx_ms`), stamp the fix with its
   own GPS time on the system clock (`core::fixtime::Clock`, removing the
   receiver's varying latency), feed the lap timer and the sector timer,
   update the saved track's record if a sector or lap improved it. A
   receiver silent past the gap gate is told to the timer as "no fix" so a
   lap across a dropout is marked rather than scored. Then per pass: lap
   peaks, `lap.advance(now)`, and on a lap event: start/finish the delta's
   lap, append to `LapHistory` (count last), clear the lap trace. Publish
   `lap`.
10. **Traces**: one point per new fix, at most 10 Hz (spaced by the fixes'
    own times), to the trip and lap traces and, while timing, the delta
    matcher.
11. **Blackbox row** if the log decimator says so (Scheduled passes, or
    forced on a lap event because the lap marker rides in that record).
12. **Detail log**: this pass's events, and on Scheduled passes the 50 Hz
    `_det` row.

**Scheduled vs Event passes** (`LogicPass`): anything that integrates over
time takes the real `now` and does not care; anything needing an even
cadence (blackbox rows, gear-calibration windows, stunt points) runs on
Scheduled passes only.

## 7.5 `input` — core 0, priority 9, 200 Hz

**Owns**: the input-event queue (producer), `Deps::buttons_held`, pod
health, `SystemInfo::pod_recoveries`/`pod_stuck`.

Every 5 ms (`vTaskDelayUntil`): read the pod's pin mask, OR in the console's
`hold` mask, publish the raw held state for Doom, run
`core::input::Decoder::update()` (debounce: a reading must be stable 4 ms,
two polls; long press at 600 ms; C and D repeat after 500 ms every 120 ms;
B, C, D act on the press, E on the release, A on the press with a long
press following), and `post_input_event` each resulting event.

Pod failure handling is the interesting part: a run of failed reads (by
time, 200 ms) recovers the bus (`ButtonPod::recover_bus`: up to nine SCL
clocks by hand, then a STOP, then re-init). After three recoveries that do
not bring it back, the pod is treated as unplugged and only probed once a
second (an absent pod used to be a 10 ms timeout every 5 ms and the bus
driver torn down five times a second). The configuration registers are
rewritten periodically, and on a pod coming back, before its first read is
believed (a fresh MCP23017 has no input inversion, so every released button
reads pressed).

## 7.6 `render` — core 1, priority 6, 120 Hz

**Owns**: the panel, the `ui::App` (on its stack), the band buffer
(internal RAM, 38 kB), the `ui::Shadow` (PSRAM, 300 kB), `hr_history`
(through the App), Display health, `render_hz_x10`, `first_frame_ms`.

Loop (3.3 (d)): wait for a button or the next deadline; hand button events
to `app.on_input()`; if a Doom-style program owns the panel, draw nothing;
take a panel re-init if `keeper` asked; refresh the lap history pointer;
`snapshot()` the slots into a Frame; `app.update(frame)`; then either a full
repaint (in 40-line bands, each drawn and sent only where it differs from
the shadow if the shadow is valid) or just the damaged rectangles
(`app.damage()` → `blit_damage` per rectangle); and once a second, the
**scrub**: resend one band from the shadow regardless (all of it every eight
seconds), which repairs a panel that lost its memory in a brown-out without
any write failing. Part 9 explains the drawing pipeline.

## 7.7 `keeper` — core 1, priority 3, 20 Hz health / 1 Hz the rest

**Owns**: the `health` slot (the only publisher), the `elec` slot, the
retry policy, NVS writes of settings, odometer, hours and waypoints,
`Deps::backlight_pct`, most of `SystemInfo`, the 10 s console summary.

Every 50 ms: `tracker.tick()`, `claim_retry()` for each device (setting
`retry_requests` bits), `publish_if_changed()` (republished on change, and
at least once a second so DIAG's last-ok times move). Once a second:

- **If `still_for_flash()`**: write changed settings, the odometer (if it
  moved ≥ 0.01 mi), engine hours, changed waypoints; commit. A flash write
  freezes core 0, so never while moving or revving.
- VIN through `IRailSense`, plus the trim, into `elec`.
- Heap, PSRAM, uptime, die temperature (`drivers::ChipTemp`), stack
  high-water marks (`watch_stacks`), into `SystemInfo`; every 10 s, the
  console summary line (docs/console.md).
- The backlight level: CONFIG brightness, dimmed after dark if Dim at night
  is on (`core::sun` from the GPS date, time and position), into
  `backlight_pct` for `ctrl` to send.
- At boot, once: whether a core dump is in flash and which task made it
  (`crash_task`, `crash_new`).

## 7.8 `power` — core 1, priority 22, 1 kHz

**Owns**: the `battery` slot, `Deps::power_fail`. Never run on the board
yet (bringup §8.1).

Every millisecond: one fast VIN conversion into `core::power::FailDetector`
(armed once VIN has been above 10.5 V for 20 samples; trips below 9.0 V for
2 samples; recovers above 10.5 V for 50; and if it is still alive 1 s after
a trip with VIN low and steady, it is on USB and lets go). Every 10 ms, the
mean and minimum of those samples go to `core::battery` (rest voltage,
crank dip, charging voltage, verdict).

On a trip, in order of what each buys: blank the backlight and lamps
directly (`Pca9955b::blank_lamps`, two short I2C writes: this roughly
doubles the hold-up time, ~9 ms to ~17 ms); set `power_fail` (logger stops
starting writes, `ctrl` keeps everything dark); save odometer and hours to
NVS, at once when parked, or once VIN has stayed down a few more ms when
moving (a glitch that comes back must not freeze core 0 mid-pull). It
cannot flush the log file: that needs ~50 ms and the board has less than
half of it. On recovery, `power_fail` clears and `ctrl` resends everything.

## 7.9 `logger` — core 1, priority 2

**Owns**: every file under `logs/` and `detail/` on the card, the
`log_flush_ack` handshake, `SystemInfo::log_records`/`log_dropped`.

Drains the blackbox ring and the four detail rings into per-stream RAM
blocks (8 kB) and writes a block when it is full or its oldest row is 2 s
old; syncs 5 s after the first uncommitted write or when the bike stops
(`core::log::FlushPolicy`). It also writes `_diag` itself, a 1 Hz row read
off the slots and `SystemInfo`, including the card's own write times. Files:
`logs/sNNN.csv` (renamed to `logs/YYYY-MM-DD_sNNN.csv` once the GPS gives a
date; the session number is one past the highest on the card), and
`detail/<stem>_imu.csv`, `_tach`, `_det`, `_evt`, `_diag`. Each detail file
opens with the firmware version, the session and the reset reason; `_evt`
also with every setting. It stops starting writes while `power_fail` is set.

Why priority 2: 3.11. Why behind a ring: an SD card can stall for hundreds
of milliseconds, and no producer may ever wait on that. A full ring drops
the newest record and counts it, rather than blocking or overwriting the
start of the incident.

## 7.10 `storage` — core 1, priority 1, 2 Hz

**Owns**: the card's mount state, `SystemInfo::sd_ejected`, the SD health
(as "mounted"), the `storage` side of the track mailbox. Mounts the card at
boot and when it is reinserted (debounced so a card jiggling over whoops
does not remount four times), notices removal, and on
`StorageRequest{Eject}` asks `logger` to flush (`log_flush_req`, waits for
`log_flush_ack`), unmounts, marks SD absent and shows "safe to pull" until
the card comes out. It serves the track mailbox (`serve_tracks`: List,
Save, Load, Forget against `tracks/tNN.trk`). USB mass storage would live
here and is not written.

## 7.11 `ota` — core 1, priority 2, event driven

**Owns**: the `ota` slot, and the only writes to the dev app slot. At boot
it reads the bootloader's rollback state for the running slot and decides
the role (`core::ota`): **stable** if the image is marked valid (or has no
state), **dev** if it is pending. A dev image never marks itself valid, so
any reset returns to stable. It then waits for `ota_request` and
`ota_upload`:

- **InstallSd**: read `update/dash.bin` from the card, check its header
  (chip, project, size, board word: an image for the other board is
  refused), write the dev slot a sector at a time, and let
  `esp_ota_end` verify the image hash. Refused unless parked
  (`core::ota::may_write`).
- **Upload**: the same, reading from `Deps::ota_stream` (3.6).
- **BootDevOnce**: set the dev slot to boot and restart. The bootloader
  marks it pending.
- **Promote** (only from inside a running dev image, parked):
  `esp_ota_mark_app_valid_cancel_rollback()`: dev becomes stable now, with
  no restart.

## 7.12 `ble` — core 1, priority 4, event driven

**Owns**: the `media`, `notify`, `heart` and `ble_diag` slots, Phone and
BleHost health. Brings up ESP-Hosted to the C6, compares the C6's firmware
with the image in the `slave_fw` partition and pushes it over SDIO if they
differ (`c6_firmware.cpp`, `core/c6`), then starts NimBLE on core 1. It
advertises the Dash Media GATT service; an Android phone writes UTF-8 lines
to its Feed characteristic (`T` title, `A` artist, `L` album, `S` state,
`P` position, `D` duration, `Y` lyric, `N` notification), which
`core::media::apply_line()` turns into `Media`/`Notify`. An iPhone is
detected by its AMS/ANCS services and read as a GATT client instead
(`core/media/apple`), feeding the same functions. Feed requires an
encrypted, bonded link; `core::ble` decides who may bond (the first phone
freely, later ones only inside CONFIG's 60 s pairing window; a link not
encrypted within 30 s is dropped). With CONFIG → Heart rate on, it is also a
central: it scans for the standard Heart Rate Service, `core::hr::Picker`
chooses a sensor, it subscribes to 0x2A37 and publishes `heart`. It acts on
`media_ctrl` (sends the rider's prev/play/next to the phone over Ctrl, or as
AMS remote commands) and `ble_request`. NimBLE's callbacks only queue (3.5).
A C6 that stops answering is heard as an ESP-Hosted transport-failure event
and reported as radio lost; the dash does not reboot.

## 7.13 `wifi` — core 1, priority 3, 4 Hz

**Owns**: the `wifi` and `ota_upload` slots, the HTTP server. Off until a
`WifiRequest{Toggle}`. Then the C6 runs a WPA2 soft AP, `CRF250F-Dash`, with
a new random eight-digit password (in the `wifi` slot for DIAG → Update),
and `esp_http_server` serves:

| Route | Does |
|---|---|
| `GET /` | the single page (HTML/JS compiled into `wifi_page.cpp`) |
| `GET /api/status` | versions, slots, card |
| `GET /api/logs`, `GET /logs/<name>`, `DELETE /logs/<name>` | list, download, delete ride logs (names checked by `core::wifi::valid_log_name`; the open log answers 409) |
| `GET /api/settings`, `POST /api/set?key=&value=` | every CONFIG item; a change is posted as `ConfigSet` |
| `GET /fw/stable.bin`, `GET /fw/dev.bin` | firmware backups, straight from the partitions |
| `POST /fw` | upload into the dev slot, through `ota_stream` to `ota` |
| `GET /api/games`, `PUT /games/<name>`, `DELETE /games/<name>` | Doom WADs in `games/` |

It turns off after 10 idle minutes or as soon as the bike moves
(`core::wifi` policy), never writes flash itself, and cannot boot anything.
It logs free internal RAM at each state change, because the SDIO link, lwIP
and NimBLE all draw on it.

## 7.14 The console task — core 1, priority 3

`main/serial_buttons.cpp` reads lines from the USB-Serial/JTAG console:
`btn X` / `long X` post decoded presses into the input queue; `hold <mask>`
feeds held buttons to `input` (`services::set_console_buttons`); `pod
trace`; `c6 hold` / `c6 release`; `panic`. `docs/console.md` has the full
list. It exists so the board can be driven from a laptop without the pod
(`tools/pod_keys.py`).

---

# Part 8 — The `core/` modules

`components/core/` is plain C++17 with no ESP or FreeRTOS headers. Each
module is a directory under `include/core/<module>/` with its source in
`src/`, and one host test, `test/test_<module>.cpp`. Every function that
needs the time takes `now_ms` (or a `t_ms` field in an input struct).
Modules are written as **stateful classes with an input struct and an
output/publish method**, so a service's job is: fill the input from slots
and drivers, call `update()`, publish the result. A service that starts
doing arithmetic is a sign the arithmetic belongs here.

Typical shape:

```cpp
namespace core::session {
struct Input  { uint32_t t_ms; float speed_mph; bool engine_on; bool imu_ok;
                uint32_t land_ms; uint32_t airtime_ms; float land_g; };
class Tracker {
public:
    void update(const Input& in);            // advance with one sample
    state::Session publish() const;          // what goes in the slot
};
}
```

The catalogue, grouped by what they do. "Run by" is the task that owns the
instance; "→" is the slot it feeds.

## 8.1 State and plumbing

| Module | Purpose | Run by |
|---|---|---|
| `state/` | `Published<T>`, `Slots`, `Frame`, `snapshot()`, `TrackBuffer`, `Command`; `to_published()` turns the lap timer's state into a `Lap` | everyone |
| `config/` | `Settings`, the `ConfigItem` table, clamp/get/set/nudged/format, views, `validate_table()` | `logic` writes; all read |
| `health/` | `Tracker`, `Policy`, `worst()` | all report; `keeper` derives |
| `flash/` | `still()`: may flash be written now | `keeper`, `ble`, through `still_for_flash` |
| `units/` | imperial/metric conversion and **all** display formatting (`speed_for_display`, `distance_for_display`, lap-time strings) | screens |
| `stack.hpp` | `DASH_BIG_FRAME_BEGIN/END` | — |

## 8.2 Sensors to numbers

| Module | Purpose | Run by → slot |
|---|---|---|
| `tach/` | `Meter`: spark pulses → rpm from edge intervals, a time-constant smoother, a refractory model, `RateEstimator` (rpm slope, for the lamp lead), `WheelMeter`: pulses → mph | `ctrl` → `engine`, `motion` |
| `shift/` | `pattern()` rpm → 8-lamp mask with hysteresis; `lamp_rpm()` (lead time); `over_limit()`; `Flasher`; `PeakHold`; `hold_lamp()` | `ctrl` → `engine` |
| `gear/` | `estimate()`: nearest gear by rpm/mph against the table | `ctrl` → `engine.gear` |
| `gearcal/` | `Window` (steady stretches), `Guide` (the guided calibration), `Learner` (≤1 %/power cycle, ≤5 % from base, after 8 agreeing episodes 30 s apart) | `logic` → `gear_cal`, `Settings` |
| `nmea/` | the NMEA sentence parser (RMC, GGA, checksums, the date) and `age_fix` | `gnss` → `gnss` |
| `fixtime/` | puts each fix's GPS time onto the system clock with the lowest-latency (NTP-style) offset | `logic`, `imu` |
| `attitude/` | the lean/pitch filter (8.4) | `imu` → `attitude` |
| `jump/` | airborne below ~0.3 g, landing peak, airtime | `imu` → `attitude` |
| `fusion/` | `SpeedFilter` (8.5), `WheelCheck`, `shown_speed_mph()` | `logic` → `fused`; `ctrl` → `motion.best_mph` |
| `input/` | `Decoder`: debounce, short/long/repeat, on-press masks; the pod's pin map | `input` |
| `battery/` | rest, crank dip, charging, verdict | `power` → `battery` |
| `power/` | `FailDetector` (VIN cut), `classify()` (bike/USB/unknown), `lamp_master_pct()` (USB cap), `battery_v()` (trim) | `power`, `ctrl`, `keeper` |
| `sun/` | sun elevation from date, time, position; the night dim curve | `keeper` |

## 8.3 Riding features

| Module | Purpose | Run by → slot |
|---|---|---|
| `geo/` | distance, bearing, `guide()` (distance + bearing to a waypoint), segment crossing with sub-sample interpolation | `logic`, screens |
| `lap/` | `Timer` (arm, cross, score, gates: arm speed, min lap, fix gap), `Sectors` (two auto-placed splits, ideal lap), `Peaks` | `logic` → `lap` |
| `delta/` | lap delta against a reference trace: nearest point in a window around the last match. **A placeholder**: thresholds never tuned on real data | `logic` → `lap.delta_ms` |
| `track/` | saved tracks: `Library` of 16 `Record`s (finish line, splits, record, sector bests, reference lap), recall within 1500 ft, "same track" within 300 ft, the file encoding | `logic` + `storage` |
| `trip/` | `Computer` (odometer as `double`, trips, hours, top speed), `FuelModel` | `logic` → `trip` |
| `session/` | the ride so far | `logic` → `session` |
| `perf/` | 0–30/0–60, 60 ft/330 ft/⅛ mile with trap speed, braking distances, interpolated thresholds, bests | `logic` → `perf` |
| `stunt/` | wheelies and stoppies: lift = pitch − path angle (climb from an inertial + GPS-altitude filter), detection thresholds, scoring, combos, grades | `logic` → `stunt` |
| `waypoint/` | the five named waypoints, START's staleness rule, their NVS encoding | `logic`, `keeper` |
| `slip/` | wheelspin/lockup: each wheel against road speed, held at its peak 0.7 s | `ui::App` (per Frame) |
| `alert/` | `Alerter`: which single alert to show (onset and clear delays, a dismissal that holds until the fault genuinely clears) | `ui::App` (per Frame) |
| `sweep/` | the start-up gauge sweep, applied to a copy of the Frame | `ui::App`, `ctrl` (lamps) |
| `nav/` | `step(state, event, layout, focus_count)`: mode/screen/focus navigation | `ui::App` |
| `hr/` | heart rate: the 0x2A37 parser, the advert parser, `Picker`, `Session`, zones, `History`, `Recovery`, `fresh()` | `ble`, `logic`, `ui::App` |

## 8.4 Phone, radio, updates, logging

| Module | Purpose | Run by |
|---|---|---|
| `media/` | the Feed line parser (`apply_line`, `apply_notify`): bounded, UTF-8-safe, `P` as an anchor; `media/apple`: AMS and ANCS into the same calls | `ble` |
| `ble/` | which phone may connect and bond (`judge`), the pairing window, the 30 s encrypt deadline | `ble` |
| `c6/` | whether the C6's firmware matches `slave_fw` (version and build id), and whether a dev image may move it | `ble` |
| `ota/` | stable/dev role from the bootloader's state, image header and board checks, `may_write` (parked) | `ota` |
| `wifi/` | the hotspot's policy: idle and moving shut-off, password, `valid_log_name`, `valid_game_name` | `wifi` |
| `log/` | the blackbox `Record`, `Ring`, CSV formatting, `Decimator`, `FlushPolicy`, file naming and session numbers; `detail.hpp`: the five detail streams, their rows and rings, `EventWatcher` | `logic`, `logger`, producers |

## 8.5 Two modules worth understanding in depth

**The lean filter (`attitude/`).** An accelerometer cannot measure lean on a
motorcycle: in a steady turn the bike leans into the resultant of gravity
and centripetal force, so "down" still points through the seat. The filter
integrates the gyros (fast, but they drift) and continuously corrects
towards a reference that does not care about the resultant:

```
lean ≈ atan( v · Ω / g )
```

with `v` the road speed and `Ω` the *world's* yaw rate, recovered from the
body gyros through the Euler-rate transform (the yaw gyro alone tilts with
the bike and reads only Ω·cos(lean)). The correction rate is capped
(`max_correction_dps`) so a rock strike cannot snap the gauge. Because the
IMU is on the handlebar, a turned bar reads as lean; the filter estimates
the steady-corner steering angle from yaw rate, speed, rake and wheelbase
and rotates it out. Because a sliding rear makes the heading turn faster
than the path, each GPS fix compares the gyro's heading change with the GPS
course change; when they disagree the bike is sliding (or airborne) and the
correction holds until they agree. Below 5 mph `v·Ω` collapses and the
accelerometer takes over. Pitch is the same filter on the other axis, with
the accelerometer trusted only near 1 g.

**The speed filter (`fusion/`, `docs/fusion.md`).** A two-state Kalman
filter (speed, accelerometer bias), predicted every pass from the IMU's
forward acceleration, corrected by GPS Doppler once per fix (noise scaled
by HDOP) and by each wheel. A correction too far from the prediction is
rejected (the gate). GPS referees the wheels: for a second after a good fix,
a wheel far from GPS is not believed (this catches a dead sensor reading 0,
which the gate alone would follow). A source rejected 1.5 s straight is
suspect; if every offered source is suspect, the filter restarts on GPS.
`WheelCheck` faults a wheel reading 0 for 5 s while the bike moves.
`shown_speed_mph()` is the policy for the big number. `tools/replay` runs the
same class over a log or a synthetic ride.

---

# Part 9 — The UI

`components/ui/` builds on the host and on the device. It has no ESP header
(enforced by `test/check_no_esp_headers.sh`). The layers, bottom up: a
**Canvas** to draw on, **Widgets** that own a rectangle and a value,
**Screens** that own widgets and map a Frame onto them, and the **App** that
owns navigation, the one live screen and the overlays, and decides what
must go to the panel.

## 9.1 Geometry, colour, fonts

- `ui/geometry.hpp`: `Rect{x, y, w, h}` (with `inset()`, `bottom()`, ...),
  `kScreenW = 480`, `kScreenH = 320`, `Color` (RGB565, `uint16_t`) and the
  raw palette (`color::kAccent`, `kDim`, `kWarn`, `kAlarm`, ...).
- `ui/theme.hpp`: the *meaning* of colours (`theme::rule`,
  `theme::secondary`, ...). Use the theme, not raw colours, so a palette
  change is one file.
- `ui/chrome.hpp`: shared furniture (titles, rules, framed tiles,
  `chrome::kRadius`, `chrome::hr_readout()`), so fifteen screens do not each
  invent a labelled box.
- Fonts: one 5×7 bitmap font (`font5x7.cpp`) drawn at integer scales. At
  scales 3–6, 8, 10, 20, 22, `FrameBufferCanvas::draw_text` blends 4-bit
  anti-aliased glyphs of Noto Sans Condensed Bold (`font_aa.cpp`, generated
  by `tools/gen_font.py`) *into the bitmap's own cell*, so text widths and
  layouts are identical either way. `text_width_px(text, scale)` measures
  without a canvas.

## 9.2 Canvas

`ui::Canvas` (`ui/canvas.hpp`) is an abstract drawing surface: `fill`,
`fill_rect`, `draw_rect`, rounded rectangles, triangles, `fill_arc` (angles
clockwise from 12 o'clock, like a gauge), lines, circles, `draw_text(x, y,
text, color, scale, align)`, 1-bpp bitmaps, RGB565 images, and a clip.

`ui::FrameBufferCanvas` implements it over a plain `Color*` buffer. On the
device the buffer is **one 40-line band** of the screen (480 × 40 pixels,
38 kB of internal RAM), not a whole frame; `set_window(x, y, w, h)` points
the canvas at a region of the screen, so drawing calls use whole-screen
coordinates and anything outside the window is clipped away. In `test_ui`
the buffer is a whole 480 × 320 frame in an array, which is what lets a
test read back pixels.

## 9.3 Widgets

`ui::Widget` (`ui/widget.hpp`):

```cpp
class Widget {
public:
    Rect bounds;
    bool dirty = true;
    virtual void render(Canvas& c) = 0;
    virtual int  damage(Rect* out, int max) const;   // default: { bounds }
    void set_bounds(const Rect& r);
    void touch();                 // the whole widget must be resent
    virtual void clear_dirty();
protected:
    void touch_part();            // part of it; damage() says which
    bool assign(float& field, float v);              // set + touch on change
    template <class T> bool assign_t(T& field, T v);
};
```

The contract:

- A widget **stores its value** and has a `set(...)` that compares with the
  stored value and calls `touch()` *only if it changed*
  (`assign`/`assign_t`). That is the dirty-flag optimisation, in one place.
  A screen can therefore call `set()` with the same value 120 times a second
  and nothing is drawn.
- `render(canvas)` draws the widget completely inside `bounds`, from its
  stored state. It may be called with the canvas windowed onto any part of
  it; it must draw the same pixels either way.
- `damage()` may report a *smaller* set of rectangles than `bounds` when the
  widget knows only part changed: a `Table` reports its changed rows, a
  `TachBar` the rows its marker crossed. `render()` then runs with the
  canvas windowed onto each rectangle.
- **Anything that changes on screen must be a widget.** A screen's
  background (`draw_background`) is drawn only on a full repaint; live data
  drawn there would freeze between repaints.

The 23 widgets (`ui/widgets.hpp`): `Label`, `ValueBox` (label + number +
unit in a box), `BigNumber` (huge digits: speed, gear), `Bar`,
`StatusDot`, `ShiftStrip` (mirrors the lamps), `DeltaBar`,
`LeanIndicator`, `Sparkline`, `Table`, `MapView` (walks an
`IPointSource`), `TachBar`, `RevBar` (RACE's full-width bar),
`LeanGauge`, `MediaRow` (an icon and a scrolling line), `MediaProgress`,
`AlbumArt`, `AlertOverlay`, `NavArrow`, `SlipBar`, `HrGraph`, `DevTag`,
`Toast`.

`ValueBox`, for example: `configure(label, unit, decimals, digits)` once in
`enter()`, then `set(value, valid)` every update. `valid = false` shows
`--`: this is how "device not ok" becomes `--` on screen.

## 9.4 Screens

`ui::Screen` (`ui/screen.hpp`), the full interface:

| Method | Called | Must |
|---|---|---|
| `name()` | for titles and diagnostics | return a constant string |
| `enter(Canvas&)` | once, when the screen is built | lay out widgets (`set_bounds`, `configure`), fill the widget list |
| `leave()` | before it is destroyed | (rarely needed) |
| `update(const Frame&)` | every frame | **set widget values only. Never draw.** Never read a slot |
| `draw_background(Canvas&)` | on a full repaint, under the widgets | draw static chrome (frames, labels, small print) |
| `wants_title()` | at build | false for screens that want every pixel (SPEED, RACE) |
| `handle(const InputEvent&)` | on a button the screen gets first refusal on | return true if consumed; post commands with `post(...)` |
| `widgets()` | by the App | return a `WidgetList{pointer, count}` of every widget |
| `focus_count()`, `set_focus()`, `focus_now()` | for screens with a cursor | report rows, follow nav's cursor, or report where it moved its own |

What a screen may use: the `Frame` passed to `update`, and through `ctx()`
the read-only `Settings*`, the `ICommandSink` (via `post()`), the
`SystemInfo*`, and the `IProgram*`. `units()` returns the unit system.

A complete, real example: TRIP (`ui/src/screens_cruise.cpp`):

```cpp
class TripScreen final : public Screen {
public:
    const char* name() const override { return "TRIP"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override { return WidgetList{list_, 7}; }
private:
    ValueBox odo_, trip_a_, trip_b_, fuel_, hours_, top_;
    Label    hint_;
    Widget*  list_[7] = {};
};

void TripScreen::enter(Canvas&) {
    odo_.set_bounds(kTripOdo.inset(8));  odo_.configure("ODOMETER", "mi", 1, 5);
    ...
    list_[0] = &odo_; ... list_[6] = &hint_;
}
void TripScreen::update(const Frame& f) {
    const char* dunit = "mi";
    odo_.set(u::distance_for_display(f.trip.odo_mi, units(), &dunit), true);
    odo_.set_unit(dunit);
    ...
}
bool TripScreen::handle(const core::nav::InputEvent& e) {
    if (e.button == Button::E && e.press == Press::Long)        // long, so a bump cannot reset it
        return post(core::state::Command(core::state::CommandId::TripReset, int32_t{0}));
    return false;
}
void TripScreen::draw_background(Canvas& c) {
    for (const Rect& r : {kTripOdo, kTripA, ...}) c.draw_round_rect(r, chrome::kRadius, theme::rule);
    c.draw_text(kTripFuel.x, kTripFuel.bottom() + 4, "estimate", theme::secondary, 1, Align::Left);
}
```

Layout is `static constexpr Rect` constants at the top of the screen's file
(`kTripOdo`, ...). There is no layout engine.

Screens live in `screens_cruise.cpp`, `screens_track.cpp`,
`screens_stat.cpp`, `screens_perf.cpp`, `screens_diag.cpp`,
`screen_config.cpp`, `screen_gearcal.cpp`, declared in `ui/screens.hpp`.

## 9.5 The App

`ui::App` (`ui/app.hpp`, `ui/src/app.cpp`) is constructed by `render` with a
`ui::Context` (settings, command sink, system info, program) and holds:

- **The navigation state** (`core::nav::State`: mode, screen, focus) and the
  `Layout` (screens per mode: CRUISE 7, TRACK 5, STAT 6, DIAG 9, CONFIG 1).
- **One live screen**, constructed with placement-new into a byte buffer
  sized and aligned for the largest screen type (`AllScreens =
  ScreenStorage<SpeedScreen, MediaScreen, ...>`). `build(id)` destroys the
  old screen and builds the new; `screen_for(mode, index)` maps navigation
  to a `ScreenId`. This keeps the App about 1.9 kB on render's stack instead
  of 13 kB for all screens at once. Consequence: **nothing a screen keeps
  survives leaving it.** What must survive (map traces, lap history, CONFIG's
  place during gear calibration, the session's lean peaks) is held by the
  App, by `logic`, or in a slot, and handed back at build.
- **The overlays**: `AlertOverlay` (driven by `core::alert::Alerter` every
  Frame), `SlipBar` (`core::slip::Meter`), `Toast` (the bottom strip for
  calls, messages, tricks, perf bests, a refused Set line: the App compares
  each event slot's `seq` with the last it showed), and `DevTag` (shown
  while `frame.ota.role` is dev).
- **The splash and the sweep** (Part 6).

`App::on_input(event, canvas)`, in order of who gets the button:

1. An **alert** that is showing takes E (dismiss).
2. A **clearable toast** (a text message) takes short E.
3. The **current screen's** `handle()`.
4. **DIAG's menu** logic (E opens the page under the cursor, B returns to
   the menu).
5. **`core::nav::step()`**: A next mode (long A: CRUISE screen 0), B next
   screen, C/D move focus. A mode or screen change builds the new screen.

`App::update(frame)`, every frame: feed the heart-rate history; run the
alerter, slip meter and toast logic; notice a dev image; swap CONFIG for the
gear-calibration page if a run opened; run the splash or the sweep (handing
the screen a modified *copy* of the Frame); otherwise
`current().update(frame)`.

## 9.6 From widget to glass: the damage pipeline

The render task decides each frame whether a **full repaint** is due
(`app.needs_full_repaint()`: the first frame, a screen change, a dev image
starting, the splash ending, a panel re-init or a failed write), or only
**damage**.

**Full repaint.** For each 40-line band, top to bottom: window the canvas on
it, `app.render(canvas, true)` (background, chrome, every widget, overlays),
then send it. If the shadow is valid, only the parts that differ from what
the glass already shows go out (so a screen change costs 6–26 ms instead of
31); otherwise the whole band goes and is recorded into the shadow.

**Damage.** `app.damage(out, max)` lists this frame's rectangles: for every
dirty widget, its `damage()` rectangles; plus any **region** an overlay has
just uncovered (a dismissed alert, a cleared slip bar, a toast that went
away), which is repainted from the ground up. For each rectangle,
`blit_damage` splits it into strips that fit the band buffer, calls
`app.draw_damage(canvas, d)` with the canvas windowed on the strip (it draws
the widget and puts any overlay that overlaps back on top), and sends the
strip through the shadow. When all are sent, `app.clear_damage()` clears the
dirty flags. If a blit fails, the shadow is invalidated and the next frame
is a full repaint.

**`ui::Shadow`** (`ui/shadow.hpp`) is a 480 × 320 RGB565 copy of what the
panel holds, in PSRAM. `diff(rect, pixels, out_parts, max, overhead_px)`
remembers the new pixels and returns the sub-rectangles that actually
differ, merging rows when sending a few unchanged pixels is cheaper than
starting another transfer (`ui::wire::overhead_px` models a transfer's cost).
A speed going 47 → 48 redraws the whole readout into the band but sends
little more than the changed digit. CPU is traded for bus time: SPEED's
riding frame is about 1 ms on the wire.

**The scrub.** Once a second `render` resends one band straight from the
shadow (all eight bands every 8 s, 3.9 ms of bus a second). An ILI9488 can
lose its memory in a brown-out without any write failing; this repairs it.

**Why test it.** `test_ui` checks that a partial update lands exactly the
same pixels as a full repaint, for every screen. That single property
catches a widget whose `damage()` under-reports, and live data drawn in a
background. `sim/wire_report` prices each widget's bus cost through the same
`ui::Shadow` and `ui/blit_plan.hpp` the firmware uses.

## 9.7 Navigation map

| Mode (A cycles) | Screens (B cycles) |
|---|---|
| CRUISE | SPEED · MEDIA · TRIP · MAP · NAV · MINIMAL · PHONE |
| TRACK | RACE · LAP · LAPS · MAP · SESSION |
| STAT | ALL · ENGINE · IMU · PERF · STUNT · HEART |
| DIAG | a menu → SENSORS · GPS · FUSION · SYSTEM · POWER · BUS · RADIO · UPDATE, and Doom |
| CONFIG | one screen generated from the `ConfigItem` table, its groups as submenus |

`docs/features.md` §6–§10 describes each screen's contents.

---

# Part 10 — End-to-end flows

Each flow below crosses several tasks. Following them is the fastest way to
see how the pieces fit together.

## 10.1 A spark to a shift lamp (the 3 ms path)

1. The spark plug fires. The wrap on its lead induces a pulse on
   `TACH_SIG` (GPIO21), through R34/C19 and a clamp.
2. **ISR** (`engine_sense.cpp`, `IRAM_ATTR`): outside the refractory window
   (`bsp::kTachRefractoryUs`, 1500 µs, to swallow ringing)? Count it, stamp
   its time in µs, `vTaskNotifyGiveFromISR(ctrl)`, `portYIELD_FROM_ISR`.
3. **`ctrl`** (core 0, priority 18) is switched in as the ISR returns. It
   latches the counters, updates `tach::Meter` from the edge interval,
   computes `shift::pattern()` (lead, hysteresis, peak hold, flash).
4. If the mask changed: one I²C write to the PCA9955B's eight PWM registers
   (~0.1 ms at 1 MHz). The lamp changes.
5. Then, not before: publish `engine` and `motion`. A screen sees the new
   rpm on render's next frame (≤ 8.3 ms later) through the Frame, and its
   `ShiftStrip` mirrors `engine.shift_mask`, so the screen and the lamps
   agree.

Nothing on this path touches a queue, a mutex, the card, flash, or core 1.
The detail log's `_tach` rows record edge-to-wake and edge-to-lamp latency
per window so a ride proves the budget (`tools/ridelog`).

## 10.2 A GPS fix to a lap time

1. The receiver sends RMC and GGA sentences for one fix over UART1.
2. **`gnss`** wakes on the bytes, `core::nmea` parses them, the first
   sentence of a new UTC second stamps `rx_ms`. Publish `gnss`;
   `wake_logic(d)`.
3. **`logic`** runs an Event pass. It sees `g.rx_ms != last_lap_rx_ms` (a
   new fix), stamps it with `fixtime::Clock::stamp(utc_ms, rx_ms)` (the
   fix's own time on the system clock, latency removed), and calls
   `lap::Timer::update(fix)`.
4. The timer checks the segment from the previous fix to this one against
   the finish line (`geo::` segment intersection, interpolated to the
   crossing point between the two fixes), and the gates (arm speed, minimum
   lap, no long gap). A crossing ends a lap: `Event::Completed` with
   `lap_ms`.
5. `logic` appends to `LapHistory`, offers the lap to the saved track's
   record, sets `last_done_ms` and `last_was_best`, publishes `lap`, and
   forces a blackbox row carrying the lap marker.
6. **`render`**'s next Frame shows the new LAST (flashing if a best) and a
   toast; `logger` writes `# Lap 3: 00:01:12.45` between the right two rows.

## 10.3 A button press to a new screen

1. The rider presses B. The MCP23017's pin goes low.
2. **`input`** reads the pod at its next 5 ms poll, the next poll confirms
   it (4 ms debounce), `Decoder` emits `{B, Short}` on the press (B is in
   `on_press_mask`), `post_input_event()`.
3. **`render`** is waiting in `wait_input_event()`; the queue receive
   returns at once. `app.on_input(e)`: no alert or toast takes B; the
   screen does not handle B; `nav::step()` advances the screen;
   `enter_current()` destroys the old screen, builds the new one, calls
   `enter()`. A full repaint is now due.
4. Same pass: `snapshot()`, `app.update()`, a full repaint through the
   shadow. The press is on the glass about 6–30 ms after it settled.

## 10.4 Changing a setting

1. CONFIG → Wheels → Front circ., the rider presses D.
2. **`render`**: `ConfigScreen::handle()` computes
   `config::nudged(item, current, +1)` and posts
   `Command::config_set(0x0201, value)` (0x0201 is Front circ.'s permanent key).
3. **`logic`**: `config::set(*d.settings, item, value)` (clamped).
4. **`ctrl`**'s next tick reads `d.settings->front_circumference_mm` into
   its `WheelConfig`; front speed changes at once.
5. **`keeper`**, the first second `still_for_flash()` is true:
   `SettingsSaver::save_changed()` writes the key to NVS `cfg` and commits.
6. Next boot: `load_settings()` reads it back before anything is set up.

The Wi-Fi page's `POST /api/set` posts the same `ConfigSet`, so steps 3–6
are identical, and the answer it returns is the clamped value.

## 10.5 The key goes off

1. VIN falls. **`power`** (1 kHz) sees it below 9.0 V twice.
2. It blanks the backlight and lamps directly, sets `Deps::power_fail`,
   and (immediately if parked, a few ms later if moving) writes odometer
   and hours to NVS.
3. `logger` stops starting SD writes; `ctrl` keeps everything dark;
   `keeper` keeps the backlight off.
4. The capacitors run out ~9–17 ms after the cut. What was in the log rings
   and not yet synced is lost (up to ~7 s of rows, `FlushPolicy`), but the
   FAT stays consistent: an unsynced file keeps its last committed length.
5. If VIN comes back (a glitch), `Recovered`: the flag clears and `ctrl`
   resends everything within a few ms.

## 10.6 A song title from the phone

1. The Android app writes `T The Devil In I` to the Dash Media Feed
   characteristic (BLE, encrypted).
2. NimBLE's host task calls our GATT write callback, which copies the bytes
   into an `Event` and `xQueueSend`s it (timeout 0).
3. **`ble`** receives it, calls `core::media::apply_line(media, line, now)`
   (bounded to 59 bytes on a UTF-8 boundary; a new title clears the lyric
   and restarts the position anchor), and publishes `media`.
4. SPEED's `MediaRow` shows it on the next frame, scrolling if too wide.
5. The rider presses C on SPEED: `MediaCtrl{0}` → `logic` → `media_ctrl`
   slot → `ble` sees the generation move and notifies the phone over Ctrl
   (or sends an AMS remote command to an iPhone).

## 10.7 A firmware upload over Wi-Fi

1. CONFIG → Wi-Fi on/off → `WifiToggle` → `logic` → `wifi_request` →
   **`wifi`** starts the soft AP on the C6 and the HTTP server.
2. A laptop joins `CRF250F-Dash` and `POST /fw` with a `dash-*.bin`.
3. The handler publishes `ota_upload{size}` and streams the body into
   `Deps::ota_stream` (waiting for room, so the upload runs at flash speed).
4. **`ota`** checks it is parked, reads the header (chip, project, size,
   board word), writes the dev slot sector by sector, and `esp_ota_end`
   verifies the image hash. Progress appears in the `ota` slot on DIAG →
   Update.
5. Nothing on the network can boot the image: the rider presses Boot dev
   once on the dash. Any later reset returns to stable unless the rider,
   running dev, presses Promote.

---

# Part 11 — Persistence

## 11.1 Flash partitions (`partitions.csv`)

| Name | Offset | Size | Holds |
|---|---|---|---|
| (bootloader) | 0x2000 | — | second-stage bootloader, with rollback support; never updated over the air |
| (partition table) | 0x10000 | — | at 0x10000, not IDF's 0x8000, to give the bootloader room; `main.cpp` refuses any other offset |
| `nvs` | 0x11000 | 64 kB | settings, odometer, waypoints, BLE bonds |
| `otadata` | 0x21000 | 8 kB | which app slot boots, and its rollback state |
| `coredump` | 0x23000 | 116 kB | the last crash |
| `ota_0` | 0x40000 | 6 MB | an app image (stable or dev) |
| `ota_1` | 0x640000 | 6 MB | the other |
| `slave_fw` | 0xC40000 | 2 MB | the C6's ESP-Hosted firmware, pushed to the C6 over SDIO |

The table must not change on a bike in use: moving NVS erases it, and NVS
holds the odometer. Secure boot and flash encryption are off by decision
(they burn eFuses and would end USB recovery and dev-slot uploads).

**Stable and dev**: which slot is "stable" is never stored; it is read from
the bootloader's state for the running slot (`core::ota`). `idf.py flash`
erases `otadata`, so a USB flash always produces a new stable. That is the
recovery path. `radio-and-updates.md` §11.7 has the full rules.

## 11.2 NVS

Two namespaces (`dash_hal/i_storage.hpp`):

- **`cfg`** — every `ConfigItem`, keyed by its permanent `key`, written by
  `keeper` (`SettingsSaver`) when still.
- **`life`** — `kKeyOdometerMi` (0x0001, double), `kKeyEngineHours`
  (0x0002, double), `kKeyPanelModel` (0x0003), and the waypoints from
  `kKeyWaypointBase` (0x0100), three keys each (lat, lon, when). Written by
  `keeper` (when still), `power` (at key-off), `wiring` (panel model, only
  on change).

Separate so that losing settings never costs the odometer. The odometer is
a `double` because a `float` stops counting usefully around 100,000 miles.
NimBLE stores its bonds in NVS through its own store, also gated on
`still_for_flash`.

**Keys are permanent.** Never renumber a key and never reuse a retired one:
that is what lets an NVS partition written by an older firmware (or the
other slot's image) still load. `test_config` holds the list of shipped
keys and fails if one moves.

## 11.3 The SD card

FAT, mounted at boot by `storage`. Layout:

| Path | Written by | Contents |
|---|---|---|
| `logs/sNNN.csv` → `logs/YYYY-MM-DD_sNNN.csv` | `logger` | the blackbox: `# RaceRender Data`, a header row, one row per log tick, `# Lap n: hh:mm:ss.cc` tags |
| `detail/<stem>_imu.csv`, `_tach`, `_det`, `_evt`, `_diag` | `logger` | the detail log (Part 12) |
| `tracks/tNN.trk` (16 slots) | `storage`, for `logic` | saved tracks: finish line, splits, record, sector bests, reference lap |
| `update/dash.bin` | the rider | an image for DIAG → Update → Install from SD |
| `games/*.wad` | the Wi-Fi page | Doom |

The blackbox columns, in order (never reorder, only append; RaceRender maps
by name):

```
Time,Latitude,Longitude,MPH,RPM,X,Y,Lap,Heading,Gear,Lean,Pitch,GVert,LapTime,
Sats,Flags,FrontMPH,RearMPH,GpsMPH,HDOP,GpsUtc,FusedMPH,FusedSigma,AltFt,HR
```

`Time` is seconds; `X` is longitudinal g and `Y` lateral (RaceRender's
convention). Each row is one `core::log::Record`, filled by `logic` in one
place so a row is one coherent moment. A missing reading is written as 0 so
a gauge visibly drops on the video, never as the last value held.

---

# Part 12 — Testing, the simulator, the tools

## 12.1 Host tests (`test/`)

A plain CMake project, no ESP-IDF. It compiles `components/core/src/*.cpp`
into a `core` static library, `components/ui/src/*.cpp` into `ui`, and one
executable per test file. The harness is `test/check.hpp`, 40 lines:

```cpp
#include "check.hpp"
int main() {
    CASE("a 700 ms press is one long press");
    core::input::Decoder dec;
    core::input::Config cfg;
    core::nav::InputEvent ev[5];
    size_t n = dec.update(0b00010000, 0, cfg, ev, 5);       // E down at t = 0
    for (uint32_t t = 5; t <= 700; t += 5) n += dec.update(0b00010000, t, cfg, ev, 5);
    CHECK(n == 1 && ev[0].press == core::nav::Press::Long);
    return check::report("input");
}
```

`CASE(name)` labels what follows; `CHECK(cond)` and `CHECK_NEAR(a, b, tol)`
record failures with file and line; `check::report(suite)` prints and
returns non-zero on failure, which `ctest` reads. (The example shows the
shape; the real input tests are in `test/test_input.cpp`.)

What the tests cover: every `core/` module (`test_<module>.cpp`, listed in
the `foreach` in `test/CMakeLists.txt`); `test_ui` (every screen drawn
against Frames, including an all-defaults Frame where every reading must be
`--`, and partial-equals-full repaint); `test_shadow` and `test_wire` (the
bus cost model); `test_settings_store` (settings through a fake NVS); and
`test_logic_*` (`logic_tick` end to end: laps, tracks, gear calibration,
waypoints, heart-rate recovery) against fake slots and a deque of commands.
`no_platform_headers` runs `check_no_esp_headers.sh`. Shared fixtures: the
fusion scenarios in `tools/replay/scenarios.hpp` are used by `test_fusion`
too, so a number printed by `replay --scenario all` is the one the tests
hold the filter to.

## 12.2 The simulator (`sim/`)

`sim/sim_main.cpp` is a C++ process that runs the firmware's own `ui::App`,
every screen and widget, `logic_tick` (through a deque `ICommandSource`),
the real button `Decoder` and `shift::pattern()`, against a fake bike
(`sim_bike.cpp`) that produces the slots `ctrl`, `imu` and `gnss` would.
Stand-ins answer for the tasks it does not run (`ota`, `storage`'s eject,
the hotspot, a BLE phone), so every alert and DIAG state is reachable.
`sim/dash_sim.py` is the Tk front end: sliders, the five buttons (down and
up separately, so the real debounce decides what a press was), event
buttons and device-fault controls.

`build-sim/dash_sim` speaks a line protocol on stdin and writes PPM frames
on stdout (scriptable; pipe into ffmpeg):

| Line | Does |
|---|---|
| `set <name> <value>` | `speed`, `rpm`, `lean`, `pitch`, `tps`, `vbat`, `fslip`, `rslip`, `deadf`, `deadr`, `down`, `temp`, `sats`, `neutral`, `ecu`, `fix`, `auto`, `duration`, `playstate`, plus `bus_mhz`, `shift`, `phone`, `hr`, `hroff`, `wclient`, `position` |
| `btn <A..E> down` / `up` | a button edge |
| `dev <device> ok\|fail\|absent\|slow` | fault injection |
| `cmd <lapline\|laparm\|lapclear\|tripreset\|jump\|spin\|flock\|rlock\|crank\|wheelie\|stoppie>` | commands and physical events |
| `notify <kind>\t<who>\t<text>` | a phone notification through the real parser |
| `media ...` | a Feed line |
| `crash <task\|none> [new]` | a core dump stand-in |
| `quit` | — |

The FRAME line it emits carries the slip bar, the alert and the backlight
level for scripts. The sim keeps its "SD card" in `sim_card/`.
`sim/wire_report` prices each widget's bus time through the firmware's own
shadow and blit planner.

The rule behind it: **a simulator that mirrors the firmware instead of
running it drifts, and then stops being evidence of anything.** That is why
`logic_tick` was split out of its task, and why the sim links the same
sources.

## 12.3 Tools

| Tool | Does |
|---|---|
| `tools/replay` (`build-host/replay`) | runs `core::fusion::SpeedFilter` over a blackbox CSV or a synthetic ride; `plot.py` draws it |
| `tools/ridelog/ridelog.py` | reads a session off the card (blackbox + detail log) and reports, per firmware assumption, what the ride measured: drops, card stalls, reset reason, tach ringing, edge-to-lamp latency, wheel circumference error, IMU gaps and clipping, vibration spectrum, gear clusters |
| `tools/pod_keys.py` | the pod from a laptop keyboard over the USB console (`docs/console.md`) |
| `tools/imu_view.py` | live attitude from `keeper`'s USB stream |
| `tools/flash_dev.py` | writes the dev slot over USB without touching `otadata` |
| `tools/c6_uart.py` | listen to, back up, inspect and flash the C6 over its UART |
| `tools/gen_font.py` | regenerates `ui/src/font_aa.cpp` |
| `tools/make_album_art.py` | bakes album art into `ui/album_art*.cpp` |
| `tools/dash_media_peripheral.py` | a laptop BLE peripheral speaking the Feed protocol, for testing the phone app |

## 12.4 The detail log

For the first rides, beside each blackbox file, five more streams, each
from one producer through its own ring (`core/log/detail.hpp`), ~130 MB an
hour, on by default (CONFIG → Log → Detail log):

| File | Rate | Producer | Contents |
|---|---|---|---|
| `_imu` | 200 Hz | `imu` | every raw sample in sensor axes, the chip's timestamp, FIFO backlog |
| `_tach` | 50 Hz | `ctrl` | pulse intervals, extra/missed pulses, refractory rejects, edge-to-wake and edge-to-lamp latency, tick work time |
| `_det` | 50 Hz | `logic` | every speed source and fusion's verdict, the attitude filter's insides, stunt lift |
| `_evt` | events | `logic` (`EventWatcher`) | every command, health change, fix, lap, jump, trick, gear and engine change, power cut, ring drop; opens with the firmware, the reset reason and every setting |
| `_diag` | 1 Hz | `logger` | heap, stacks, timing, temperatures, battery, device states, rows and drops per stream, the card's own write times |

Its purpose is to test every assumption baked into the firmware against a
real ride; `tools/ridelog` is the reader.

---

# Part 13 — Recipes

Each recipe lists every place a change must reach. Missing one usually
compiles and is wrong at run time, so these are checklists.

## 13.1 A new `core/` module

1. `components/core/include/core/<m>/<m>.hpp` and
   `components/core/src/<m>.cpp`, namespace `core::<m>`. No ESP or FreeRTOS
   header; time as a parameter; no allocation; no globals. Prefer a class
   with an `Input` struct, `update()` and `publish()`.
2. Add the `.cpp` to **three** source lists: `components/core/CMakeLists.txt`
   (`SRCS`), `test/CMakeLists.txt` (the `core` library), and
   `sim/CMakeLists.txt` if the sim runs code that uses it.
3. `test/test_<m>.cpp` with `CASE`/`CHECK`, and add `<m>` to the `foreach`
   list in `test/CMakeLists.txt`. Every change to `core/` needs a test.
4. Call it from the task that owns the data (usually inside `logic_tick`),
   then publish.

## 13.2 A new field in an existing slot

1. Add it to the struct in `core/state/vehicle.hpp` with a default and its
   unit in the name, and a comment saying what 0 means.
2. The slot's **one writer** fills it before `publish()`. Check `slots.hpp`
   for who that is; never write it from anywhere else.
3. Readers get it from the Frame (screens) or `slots->x.get()` (tasks).
4. If it changes while riding and belongs on the video, also a blackbox
   column (13.8). If it helps tuning, a detail-log field.
5. If it is shown, add it to `test_ui`'s fake Frame.

The struct must stay trivially copyable: fixed `char` arrays, no pointers.
Size matters a little (it is copied every frame), so do not add arrays of
history: history is bulk data (4.6).

## 13.3 A new slot

1. A struct in `vehicle.hpp`.
2. `Published<NewThing> new_thing; // <writer>` in `Slots`
   (`core/state/slots.hpp`). The comment *is* the ownership contract.
3. If screens need it: a member in `Frame` and one line in `snapshot()`
   (`core/src/state.cpp`).
4. If it is a request to another task, keep it out of the Frame, have
   `logic` publish it in response to a command, and have the receiver watch
   `generation()` (4.5).

## 13.4 A new sensor or chip (driver)

1. **Pins**: in `components/bsp/include/bsp/pins.hpp` only, following the
   board (`docs/p4-board-pin-map.md`).
2. **Bus**: if it needs a new bus or bus setting, in
   `components/bsp/src/buses.cpp`. A driver adds a device to a bus; it never
   creates one. On SPI2, give it its own clock, never call
   `spi_device_acquire_bus()`, and use polling transactions if it shares the
   bus with the SD card.
3. **Interface**: an abstract class in `components/dash_hal/include/hal/`
   (or reuse one), returning `hal::Status`, with no ESP types in it.
4. **Driver**: `components/drivers/src/<chip>.cpp` + header, added to
   `components/drivers/CMakeLists.txt`. Rule 5: report, never `abort`,
   never block forever, never retry in a loop; short timeouts.
5. **Health**: a new `core::state::Dev` entry *before* `Count` (at most 32
   devices, one bit each in `retry_requests`), its name in `dev_name()`
   (`core/src/state.cpp`), and its DIAG row comes for free.
6. **Wiring**: a static instance in `main/wiring.cpp`, a `Deps` member,
   `bring_up(Dev::X, x.init())` at the right point in the boot order (or
   `set_absent` if optional).
7. **Owning task**: one task reads it, reports `report_ok`/`report_error`
   to the tracker, publishes into its slot, and handles
   `take_retry(d, Dev::X)` between transactions. If `keeper` should retry
   it, add it to `kOwned` in `keeper_service.cpp`.
8. **Sim**: give the fake bike a stand-in so screens and logic can be
   exercised without the chip.
9. Mark anything settled only on a bench with `BRINGUP:` and add it to
   `docs/bringup.md` §10.

## 13.5 A new screen

1. Declare the class in `components/ui/include/ui/screens.hpp` (deriving
   `ui::Screen`), implement it in the mode's `screens_*.cpp`: `enter()`
   (bounds, configure, widget list), `update()` (set values only),
   `draw_background()`, `widgets()`, `handle()` if it takes buttons.
2. Add it to `AllScreens` and `ScreenId` in `ui/app.hpp`, to
   `App::build()` and `App::screen_for()` in `ui/src/app.cpp`.
3. Bump that mode's count in `core::nav::Layout` (`core/nav/nav.hpp`) and
   fix `test_nav`'s expectations.
4. `test_ui` draws every screen; add it there with a realistic Frame and
   with an empty one (every reading `--`).
5. Anything that moves is a widget; anything that must survive leaving the
   screen lives in the App or a slot, not in the screen.
6. Document it in `docs/features.md` §6–§10.

## 13.6 A new setting

1. A member in `core::config::Settings` with its default.
2. One line in the `ConfigItem` table (`core/src/config.cpp`), with a
   **new key never used before** (keys are permanent NVS IDs), its group
   (CONFIG's submenu), range, step and unit, `FIELD(member)`.
3. Add the key to `test_config`'s shipped-key list.
4. Read it where needed from `*d.settings` (or extend a view such as
   `shift_view()`); a task that copies it must re-read on change.
5. It is now on the CONFIG screen, saved to NVS, on the Wi-Fi page, and in
   the detail log's settings dump, with no further code.
6. Add it to `docs/features.md` §34.

## 13.7 A new command

1. Append a `CommandId` in `core/state/command.hpp` (append, so existing
   values do not move), and its name in `command_name()`
   (`core/src/state.cpp`) for the detail log.
2. Handle it in `logic_tick`'s switch (`services/src/logic_tick.cpp`). If
   another task must act, publish a request slot (13.3 step 4).
3. Post it from a screen's `handle()` with `post(Command(id, arg))`, or make
   it a CONFIG action (an `Action` `ConfigItem` carrying the `CommandId`).

## 13.8 A new blackbox column

1. A field in `core::log::Record` (`core/log/log.hpp`), unit in the name.
2. Fill it in `blackbox_row()` (`services/src/logic_tick.cpp`).
3. Append its name to the header string and its value to the row formatter
   in `core/src/log.cpp`, **at the end**. Never rename or move a column.
4. A missing reading writes 0, not the last value.
5. Update `test_log`, and any reader that cares: `tools/ridelog`, the Wi-Fi
   page's lap analysis and GPX export (`services/src/wifi_page.cpp`).

## 13.9 A new task

1. `start_x(Deps&)` declared in `components/services/include/services/services.hpp` and defined at the
   bottom of `services/src/x_service.cpp`, added to
   `components/services/CMakeLists.txt`.
2. Choose its core: core 0 only if it is real-time and never touches a
   radio, the filesystem or flash; otherwise core 1. Choose its priority by
   what it may preempt (3.11). On core 0, never above 18.
3. Its stack in bytes; keep frames under 1 kB; `watch_stacks` will report
   it automatically.
4. Its loop shape (3.3): every wait with a timeout; if it joins the
   watchdog, nothing blocks for seconds.
5. Call `start_x` from `app_main` at the right point (Part 6), and list it
   in `services.hpp`'s table, `CLAUDE.md` and Part 2.3 of this guide.

---

# Part 14 — Pitfalls, and a glossary

## 14.1 Pitfalls, each learned the hard way

- **Two writers on one slot.** Compiles, runs, and corrupts state
  intermittently. Check `slots.hpp`'s comment before calling `publish`.
- **A screen reading a slot or drawing in `update()`.** Breaks testability
  and the damage pipeline. Screens get a Frame and set widgets.
- **Live data in `draw_background()`.** It freezes, because backgrounds are
  drawn only on full repaints. `test_ui`'s partial-equals-full check catches
  it.
- **A widget whose `damage()` under-reports.** Stale pixels left on the
  glass. Same check catches it.
- **Anything on core 0 above priority 18**, or any blocking call in `ctrl`.
  The shift lamps go late, silently.
- **A flash write while moving** (NVS, OTA, bonds). Freezes core 0 up to
  ~20 ms. Gate on `still_for_flash()` / `core::ota::may_write`.
- **A queued SPI transaction on SPI2** beside the SD card. Deadlocked the
  IMU and the SD host and rebooted the board by watchdog. IMU transactions
  are polling; never `spi_device_acquire_bus()`.
- **Re-initialising a device from the wrong task.** Lands mid-transaction.
  Use `request_retry` / `take_retry`.
- **A big local variable.** Fine on the host, overflows a 4 kB task stack on
  the board. The 1 kB frame limit is a build error for a reason.
- **A new plain static.** Eats the internal RAM available before the
  scheduler starts; enough of them boot-loop the board. Big statics get
  `EXT_RAM_BSS_ATTR`.
- **Editing `sdkconfig.defaults` and expecting the build to change.** It
  only seeds a new `sdkconfig`. Delete `sdkconfig` and rebuild; some symbols
  are derived and recomputed, so set the choice, not the int.
- **`a >= b` on two times.** Wrong across wrap-around. Use
  `static_cast<int32_t>(a - b) >= 0`.
- **Showing 0 or the last value for a dead device.** Show `--`. "0 psi"
  and "no idea" must not look the same.
- **Renumbering a config key, or moving a blackbox column.** Old NVS stops
  loading; RaceRender templates break. Keys and columns are permanent.
- **Treating `i_imu.hpp`'s old axis comment as truth.** The convention is x
  forward, **y left**, z up (`core/attitude/attitude.hpp`); that comment was
  wrong until 1.62's docs pass.
- **Assuming a driver works because it compiles.** `docs/bringup.md`'s
  status table is the only record of what has run on the hardware.

## 14.2 Glossary

| Term | Meaning |
|---|---|
| **App** | `ui::App`: navigation, the one live screen, overlays, damage |
| **BLE / GATT** | Bluetooth Low Energy; GATT is its attribute protocol (services and characteristics a client reads, writes, subscribes to) |
| **bsp** | Board support package: `components/bsp/`, pins and buses |
| **C6** | The ESP32-C6 radio chip on the module, running ESP-Hosted |
| **Command** | A request from the UI (or the Wi-Fi page) to `logic`, through the one queue |
| **core 0 / core 1** | The P4's two CPU cores; real-time island / everything else |
| **Damage** | The rectangles that must go to the panel this frame |
| **Deps** | The struct of references every task gets; the replacement for globals |
| **Detail log** | Five extra per-ride files for tuning (`detail/`) |
| **Dev slot / stable slot** | The two firmware partitions' roles, from the bootloader's rollback state |
| **ESP-Hosted** | Espressif's firmware/driver pair making the C6 a network and Bluetooth adapter for the P4 over SDIO |
| **ESP-IDF** | Espressif's SDK; `idf.py` is its build tool |
| **FIFO** | The IMU's 2 kB on-chip sample buffer |
| **Frame** | `core::state::Frame`: one snapshot of every display slot, handed to screens |
| **Fused speed** | The Kalman filter's road speed from IMU, wheels and GPS |
| **Generation** | `Published<T>::generation()`: the count of publishes, used as a sequence number |
| **HAL** | `components/dash_hal/`, namespace `hal`: abstract interfaces |
| **HDOP** | GPS horizontal dilution of precision: lower is a better fix |
| **i80 / 8080 bus** | The panel's 8-bit parallel interface |
| **IMU** | Inertial measurement unit: the ICM-42688-P gyro + accelerometer |
| **ISR** | Interrupt service routine |
| **Lean / pitch / yaw** | Roll about the bike's length / nose up-down / turning about vertical |
| **NimBLE** | The Bluetooth host stack, running on the P4 |
| **NMEA** | The GPS's text sentence format (`$GPRMC,...`) |
| **NVS** | Non-volatile storage: ESP-IDF's key-value store in flash |
| **OTA** | Over-the-air (here: card or Wi-Fi) firmware update |
| **PCA9955B** | The LED driver for the shift lamps and backlight |
| **Pod** | The five-button handlebar unit (MCP23017) |
| **PSRAM** | The 32 MB external RAM |
| **Published<T>** | The single-writer, lock-free slot type |
| **RaceRender** | The video-overlay program the blackbox CSV is written for |
| **Refractory window** | Time after an accepted tach edge in which further edges (ringing) are ignored |
| **Request slot** | A non-display slot carrying a request from `logic` to one task |
| **Scrub** | Render's once-a-second resend of one band from the shadow |
| **Shadow** | `ui::Shadow`: render's copy of what the panel holds |
| **Shift lamps** | The eight LEDs that tell the rider when to change gear |
| **Slot** | One `Published<T>` member of `core::state::Slots` |
| **Splits / sectors** | The two intermediate timing lines in a lap, and the three parts they make |
| **Still** | `core::flash::still`: every speed source near zero and the engine at most idling |
| **Tach** | Tachometer: engine rpm |
| **Toast** | The strip along the bottom for transient events |
| **WAD** | Doom's game-data file |
| **Watchdog** | The 5 s task watchdog that reboots on a hung `ctrl`/`imu`/`logic`/`render` |

---

*Written 2026-11-02 against firmware 1.62. When the code changes, update
the part that describes it; the other docs say where each fact's owner
lives (`CLAUDE.md`, "Keeping the docs indexable").*
