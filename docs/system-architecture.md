# Dashboard V2 — System Architecture

Target: ESP32-P4 + ESP32-C6 (Guition JC-ESP32P4-M3-C6 module), ESP-IDF
**v6.0.2**, FreeRTOS, C++17. The pin map is `p4-board-pin-map.md`.

This document was argued for the ESP32-S3 board and carried to the P4
board when the platform changed (`history/p4-platform-selection.md`). The
architecture did not change; the hardware sections below (§5.1, §6, §7.6,
§11.4, §16) describe the P4 board.

**Status.** This was written as a design, before any of it existed, and is
the argument for the shape of the thing. Most of it is built, and its facts
were brought up to date on 2026-11-02 (firmware 1.62). For *what exists and
how it behaves* read `features.md`; for *what has run on the board*,
`bringup.md`'s status table. Code snippets here are sketches of the idea;
the headers they name are the truth.

Not built, deliberately: **CAN / `engine_service`** (§10.1 — the board has
no transceiver), **USB mass storage** (§9.3 — the ownership seam exists,
the TinyUSB half does not), and the **TPMS scan** (§11.2 — waits on a
sensor to capture).

| § | Topic | § | Topic |
|---|---|---|---|
| 1 | The five rules | 9 | Storage, the logger, USB MSC |
| 2–3 | Layers, directory layout | 10 | Engine/ECU, why no CAN |
| 4 | State: slots, Frame, commands | 11 | BLE, Wi-Fi, partitions, OTA slots → `radio-and-updates.md` |
| 5 | Tasks, bus ownership, latency | 12–13 | Health; the config table |
| 6 | Boot order | 14–15 | Host tests; what changed from V1 |
| 7 | UI model, buttons, panel | 16 | Pin summary, hardware limits |
| 8 | What the IMU can give | 17–18 | Build order; open questions |

Decisions already made:

| Item | Decision |
|---|---|
| Framework | Pure ESP-IDF. No Arduino layer. |
| Graphics | Own retained-mode widget layer on `esp_lcd`. No LVGL. |
| Data flow | One publish slot per producer. Lock-free double-buffered read (§4.1). |
| Host tests | Yes. All logic builds and runs on the PC. |
| SD card | Config, track logs, blackbox, USB mass storage. |
| Buttons | External I2C pod on the handlebar (MCP23017). |
| Settings | A fifth mode: CONFIG. |
| ECU data | Interface now, null driver now, real driver later. See §10.1 — the dash has no CAN transceiver. |
| IMU | Full fusion: lean, pitch, jump detection. |
| Faults | Never stop. Mark the device dead and keep running. |
| BLE | In. NimBLE host on the P4, controller on the C6. Phone media and notifications (Android app or an iPhone's AMS/ANCS), the rider's watch heart rate; TPMS later. |
| Wi-Fi | Updates only, on demand. A hotspot the dash runs while parked, never a station; no stored passwords (§11.7.1). |

---

## 1. Design rules

These five rules decide every later argument.

1. **A module gets data from its arguments, not from a global.**
   The old build had `globals.h` with 60 `extern` variables. That is gone.
2. **One writer per piece of state.** No mutex is needed if only one task
   writes a value. Readers get a consistent copy, checked by a generation
   counter.
3. **Logic does not know about hardware.** Everything in `core/` compiles
   with plain g++ on your laptop. It has no `esp_` include and no `Arduino.h`.
4. **A screen is a pure function of one frame.** A screen never reads the
   sensors. It receives a `Frame` and sets widget values. Nothing else.
5. **A driver reports health. It does not decide policy.** If the GPS stops
   answering, the driver says so. The app decides what to show.

---

## 2. Layers

Data moves up. Commands move down. No layer skips a layer.

```
          ┌──────────────────────────────────────────┐
   app/   │ main, task creation, dependency wiring   │
          └──────────────────────────────────────────┘
          ┌──────────────────────────────────────────┐
    ui/   │ render task, widgets, screens, modes     │
          └──────────────────────────────────────────┘
                    ▲ Frame            │ Command
          ┌──────────────────────────────────────────┐
  state/  │ Published<T> slots, Frame, command queue │
          └──────────────────────────────────────────┘
                    ▲ publish          │
          ┌──────────────────────────────────────────┐
services/ │ tasks: ctrl, imu, gnss, logic, input,    │
          │ render, logger, storage, keeper, power,  │
          │ ota, ble, wifi                           │
          └──────────────────────────────────────────┘
              ▲ readings        │ core/ calls (pure)
          ┌───────────────────┐ ┌──────────────────────┐
 drivers/ │ chip drivers      │ │ core/ pure logic     │
          └───────────────────┘ └──────────────────────┘
          ┌──────────────────────────────────────────┐
   bsp/   │ pin map, bus setup — the only truth      │
          └──────────────────────────────────────────┘
```

---

## 3. Directory layout

```
dash/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── main.cpp                 # esp_main: build objects, start tasks
│   └── wiring.cpp               # the one place drivers meet services
├── components/
│   ├── bsp/                     # pins.hpp, buses.cpp
│   ├── dash_hal/                # abstract interfaces only, no code
│   │   ├── i_imu.hpp  i_gnss.hpp  i_engine.hpp  i_display.hpp
│   │   ├── i_gpio_expander.hpp  i_storage.hpp  i_clock.hpp
│   │   └── i_shift_lamps.hpp
│   ├── drivers/                 # one file per chip, not one directory
│   │   ├── icm42688p            # SPI, FIFO
│   │   ├── pa1616d              # UART only; the parser is core/nmea
│   │   ├── pod                  # MCP23017, handlebar, over I2C
│   │   ├── ili9488              # esp_lcd panel on the 8080 bus, RGB565
│   │   ├── pca9955b             # 16-ch LED driver: shift lamps + backlight
│   │   ├── engine_sense         # tach, both wheel Halls, neutral
│   │   ├── vin_sense            # VIN divider on ADC1
│   │   ├── nvs_storage  clock
│   │   └── sdcard               # card mount, hotplug
│   ├── core/                    # NO esp headers. Builds for host.
│   │   ├── state/               # Published<T>, slots, Frame, Command, TrackBuffer
│   │   └── alert attitude battery ble c6 config delta fixtime flash fusion
│   │       gear gearcal geo health hr input jump lap log media nav nmea ota
│   │       perf power session shift slip stunt sun sweep tach track trip
│   │       units waypoint wifi  # one directory each; features.md maps them
│   ├── services/               # one file per task, plus their helpers
│   │   ├── ctrl imu gnss logic input render logger storage keeper
│   │   │   power ota ble wifi  # *_service.cpp, the thirteen tasks (§5)
│   │   ├── logic_tick.cpp       # logic's work; the simulator calls it too
│   │   ├── queues.cpp           # the command and input queues
│   │   ├── c6_firmware.cpp settings_store.cpp track_store.cpp
│   │   ├── wifi_page.cpp stack_watch.cpp watchdog.cpp
│   │   └── engine_service/      # NOT BUILT — no CAN transceiver (§10.1)
│   ├── ui/
│   │   ├── canvas.hpp  framebuffer_canvas.cpp   # primitives, clipping
│   │   ├── widgets.hpp/.cpp     # all 23 widgets in one pair
│   │   ├── screens_{cruise,track,stat,perf,diag}.cpp  screen_{config,gearcal}.cpp
│   │   ├── geometry.hpp theme.hpp chrome.hpp    # palette, meaning, furniture
│   │   ├── font5x7.cpp font_aa.cpp icons.cpp album_art*.cpp
│   │   ├── shadow.cpp           # the copy of the glass (ui::Shadow)
│   │   └── app.cpp              # mode/screen dispatch, one live screen
│   ├── doom/  doomgeneric/      # Doom, parked only (features §31.1)
├── sim/  tools/  demo_p4/       # simulator, host tools, the demo board
└── test/                        # host build: plain cmake + ctest
    ├── CMakeLists.txt           # plain cmake, does not use idf.py
    └── test_lap.cpp  test_attitude.cpp  test_nav.cpp  ...
```

Two names differ from the sketch above and both were forced. **`hal/` is
`dash_hal/`** because ESP-IDF ships a component called `hal`, a component
name is its directory name, and a project component wins — ours silently
replaced the IDF's and the build died three components away inside
`espcoredump`. The include prefix and the namespace are still `hal`.
**`tach_capture` and `hall_capture` are one `engine_sense`**, because they
are the same three edge counters behind one latched read; `backlight` is
three PCA9955B channels that `ili9488` forwards to. The S3 board's
`mcp23s17` and FET `shift_lamps` went with that board: the PCA9955B is
both now.

`core/tpms/` does not exist yet — it waits on a sensor to capture.

---

## 4. The state model

### 4.1 Publish slots

Each producer owns one slot. One writer, many readers, no mutex.

`core/state/published.hpp`: **two buffers and a generation counter, not a
seqlock.** `publish()` writes the buffer readers are not using, word by word
in relaxed atomics, then bumps the counter; `get()` copies the buffer the
counter names and retries only if the counter moved during the copy.
`generation()` tells a reader whether anything new arrived. `T` must be
trivially copyable.

It replaced a seqlock, the design first written here. A seqlock reader that
preempts its writer on the same core spins forever, because the writer
cannot run to finish: `ctrl` at priority 18 reading `gnss` at 12 did exactly
that. With two buffers a reader never waits on a writer that stopped half
way; it reads the last complete one.

This replaces the old `g_snapMutex`, `publishSnapshot()`,
`copySnapshotToGlobals()`, and every `g_*` mirror.

Two things about the reader loop that are easy to get wrong, and that a
stress test caught:

- **The retry must be a `for(;;)`, not a `do/while(a != b)`.** In a
  do-while, `continue` jumps to the condition and compares against an
  uninitialised value. That version passed casual use and failed at about
  one read in a million under three concurrent readers.
- **A reader copying a torn value is expected and fine.** The second
  counter load is what detects it, and the copy is discarded. Do not
  "optimise" that load away.

### 4.2 The slots

The structs are in `core/state/vehicle.hpp`; `core/state/slots.hpp` lists
every slot with its one writer in a comment. As of 1.62:

| Writer | Slots |
|---|---|
| `ctrl` | `engine`, `motion` |
| `imu` | `attitude` |
| `gnss` | `gnss` |
| `logic` | `trip`, `lap`, `waypoint`, `fused`, `gear_cal`, `session`, `perf`, `stunt`, `hr_recovery`, and the requests to other tasks: `media_ctrl`, `ble_request`, `ota_request`, `imu_request`, `storage_request`, `wifi_request` |
| `keeper` | `elec`, `health` |
| `power` | `battery` |
| `ble` | `media`, `notify`, `heart`, `ble_diag`, `tpms` (once the scan exists) |
| `ota` | `ota` |
| `wifi` | `wifi`, `ota_upload` |

A command that another task must carry out (forget a phone, install an
image) is turned by `logic` into a request slot that task reads, so the
command queue keeps one drain point and every slot keeps one writer.

### 4.3 The Frame

The render task reads every slot **once** per frame, into one struct.

```cpp
struct Frame {
    Engine engine; Motion motion; Attitude att; Gnss gnss;
    Trip trip; Lap lap; Elec elec; Health health;
    uint32_t t_ms;
};
Frame snapshot(const Slots&);   // a sketch: the real Frame carries more slots
```

A screen receives a `const Frame&`. It cannot reach the hardware. This is
the rule that keeps the UI code readable and testable.

### 4.4 Bulk data is not in the Frame

Track points, lap history, and the delta reference trace are large. They do
not go in a slot. They live in append-only ring buffers with one writer:

```cpp
template <typename T, size_t N>
class TrackBuffer {          // one writer, many readers
    void push(const T&);     // writer only
    size_t snapshot_count() const;          // atomic load
    bool read(size_t i, T& out) const;      // safe if i < count
};
```

The map screen walks the buffer directly. There is no 2.4 kB memcpy per
publish, and no `bulkRev` counter to keep in step.

### 4.5 Commands going down

The UI never writes state. It posts a command.

`core/state/command.hpp`: a `CommandId` (about thirty, listed in
`features.md` §34) and a small union argument (`f`, `i`, or a config key and
value). `logic` drains the one queue.

One queue. One drain point. Easy to log every command for a bug report.

---

## 5. Tasks

Core 0 does acquisition and control. Core 1 does the display, the card and
both radios. The radios are on the C6 and their hosts are on core 1, so
core 0 is yours.

| Task | Core | Prio | Rate | Owns |
|---|---|---|---|---|
| `ctrl` | 0 | 18 | 500 Hz + every tach edge | tach math, wheel speed, shift-light output; every PCA9955B write (lamps, backlight, config, re-init) |
| `imu` | 0 | 15 | 200 Hz, INT1 | ICM-42688-P FIFO, attitude filter, jumps |
| `gnss` | 0 | 12 | on UART data | NMEA parse, position |
| `logic` | 0 | 10 | 50 Hz + on demand | lap timer, trip, fuel, command drain; woken by a new fix or a command |
| `input` | 0 | 9 | 200 Hz (`bsp::kPodPollHz`) | I2C pod poll, debounce, event queue, pod bus recovery |
| `render` | 1 | 6 | 120 Hz | frame snapshot, widgets, 8080 panel bus |
| `logger` | 1 | 2 | drain | ring buffer to SD (below the rest: IDF's SD driver spins through card busy time) |
| `storage` | 1 | 1 | — | NVS, card mount, the USB-MSC ownership seam, route and search jobs (under the logger: a route's ~1,100 card reads back to back starved it past its 3 s lock wait) |
| `keeper` | 1 | 3 | 20 Hz | health states (the one writer of `health`), retry policy (the owning task re-inits), NVS save (only when `core::flash::still`), backlight level, die temperature, the 10 s console summary |
| `power` | 1 | 22 | 1 kHz | VIN sense: the power-fail trigger (dark, stop SD writes, save totals) and the battery monitor's samples |
| `ble` | 1 | 4 | event | media Feed and Ctrl (§11), an iPhone's AMS/ANCS, the watch's heart rate; TPMS scan not written |
| `ota` | 1 | 2 | event | the stable/dev slots, the only writer of the dev slot (§11.7) |
| `wifi` | 1 | 3 | 4 Hz | the update hotspot and its web server, off until asked (§11.7.1) |

The GPS is polled at 20 ms rather than reading a UART event queue. That
was measured and left alone: 10 ms on a path whose end-to-end latency is
140 ms, which is nothing a rider can see.

The IMU is not polled. It blocks on INT1 with a timeout, and the timeout
is a fallback rather than the design — see §8. PPS is not wired on the P4
board; `bsp/pins.hpp` says why.

Thirteen tasks of your own (the `xTaskCreatePinnedToCore` in each
`*_service.cpp` is the truth for core, priority and stack). Each one has a
single job you can say in one line. The tightest stack high-water mark goes
on DIAG → System and every task's is logged at 10 s. `ctrl`, `imu`, `logic`
and `render` join the task watchdog, which panics on the bike after 5 s.
`power` is at 22 on core 1: it must act within a millisecond of VIN falling,
and core 1 has nothing that cannot wait.

The P4 has no radio. The controller runs on the module's ESP32-C6, and
the P4 runs only the NimBLE host and the ESP-Hosted transport. NimBLE is
pinned to core 1. ESP-Hosted's tasks cannot be pinned, so `ble` lowers
them below every core-0 task instead. See §11.4.

The core split therefore is: **core 0 is a real-time island** with no
radio and no filesystem in it. **Core 1 takes everything that can afford
to be late** — display, card, Bluetooth, Wi-Fi.

### 5.1 Bus ownership

- **SPI2** — IMU and SD card (SD in SPI mode). `imu` uses the IMU; the
  card is used by `logger`, `storage`, `ota` and `wifi`, under the one lock
  in `drivers/sdcard`. Sharing is safe: the IDF `spi_master` driver
  serialises access per bus. It does **not** interleave the two devices
  transaction by transaction, though. The IDF's SD-over-SPI host
  (`esp_driver_sdspi/src/sdspi_host.c`, `sdspi_host_start_command`) calls
  `spi_device_acquire_bus()` itself and holds the bus for a whole SD
  command: a multi-block write, and the polling while the card signals
  busy. So the IMU waits out every SD command, usually about a millisecond
  and, while the card is busy, tens of ms or more. The bus lock has no
  priority inheritance, so a task that preempts the card's caller (on core
  1, `render` preempts `logger`) stretches the wait. The IMU's 2 kB FIFO
  holds ~640 ms at 200 Hz and `imu` drains it all on the next read, so
  this costs latency, not samples; the `_imu` detail rows log the FIFO
  backlog per read. Our own code still never calls
  `spi_device_acquire_bus()`: a second holder would only lengthen the IMU's
  waits.
- **i80 LCD bus** — the panel only, 8-bit 8080 via LCD_CAM. Owned by
  `render`. DMA, no contention.
- **I2C0** — handlebar pod only, 100 kHz. Owned by `input`.
- **I2C1** — PCA9955B only, 1 MHz. `ctrl` is its only writer: lamps, the
  backlight (level decided by `keeper`, handed over in
  `Deps::backlight_pct`), the config refresh and the re-init. The one
  exception is `power` blanking it at VIN cut. A crushed pod cable cannot
  take the lamps with it.
- **UART1** — GPS only. Owned by `gnss`.

### 5.2 Latency budget

Tach edge to shift lamp must stay under about 3 ms, as before. Path: GPIO
edge → the tach ISR wakes `ctrl` at once (`EngineSense::notify_on_tach`), so
the lamps are decided at the edge, not at the next 2 ms tick → one PCA9955B
I2C write (~100 µs at 1 MHz). `ctrl` runs at priority 18, the highest of
ours on core 0; anything added to core 0 above 18 breaks the budget. IDF's
`esp_timer` task is at 22 there, so NimBLE uses FreeRTOS timers, and
ESP-Hosted's tasks and IDF's `sys_evt`, created above 18, are lowered at run
time (`services::demote_radio_tasks`). The render task cannot delay `ctrl`:
it is on the other core and lower priority.

**A flash write freezes core 0** for its whole length, up to ~20 ms when
NVS erases a page: IDF suspends the other core's scheduler and the P4 has
no flash auto-suspend (IDF v6.0). So nothing writes flash while the bike
moves: `keeper` and NimBLE's bond store only when `core::flash::still`,
OTA only parked, and `power` saves the totals at key-off.

---

## 6. Boot order

On the S3 board `TFT_RST` sat behind the SPI expander, which fixed the
boot order. On the P4 board it is a plain GPIO (GPIO53, R3 pull-up), so
the trap is gone. What fixes the order now is the backlight: it is three
channels of the PCA9955B, so the LED driver has to answer before the
backlight can.

1. `bsp::board_init()` — SPI2, I2C0, I2C1, UART1, and the i80 LCD bus.
2. SD card and settings — the settings decide how the rest is set up.
3. PCA9955B — comes up with every output dark. Its reset is on the P4's
   EN, so a software reset does not reset it; `init()` rewrites it.
4. ILI9488 — reset pulse, panel init, backlight to a low duty.
5. Show a boot screen at once. Do not wait for the sensors.
6. IMU, GPS, pod, engine sense, VIN sense — each reporting to `health`.
7. Start the tasks.

If step 3 fails, log it loudly, mark `led_driver` as `Fault`, and carry on.
The panel still initialises; what you lose is the backlight and the shift
lamps, which the health table already describes. `main/wiring.cpp` is
this sequence.

**Before the first power-on**, confirm the FPC pinout against the panel's
own datasheet, especially IM[2:0] (FPC 38–40, wired 011 for 8-bit 8080).
A panel in the wrong interface mode never answers — which looks identical
to a dead panel. `bringup.md` §0.

---

## 7. UI model

### 7.1 Modes and screens

```cpp
enum class Mode : uint8_t { Cruise, Track, Stat, Diag, Config, Count };

struct ModeDef { const char* name; Screen* const* screens; uint8_t n; };
```

| Mode | Screens |
|---|---|
| **Cruise** | SPEED · MEDIA · TRIP · MAP · NAV · MINIMAL · PHONE |
| **Track** | RACE · LAP · LAPS · MAP · SESSION |
| **Stat** | ALL · ENGINE · IMU · PERF · STUNT · HEART |
| **Diag** | a menu of SENSORS · GPS · FUSION · SYSTEM · POWER · BUS · RADIO · UPDATE, and Doom |
| **Config** | Generated from the `ConfigItem` table, its groups as submenus |

As built (`ui/app.cpp`, `ui::App::build()`); `features.md` §6–§10 says what
each shows. Only one screen is alive at a time, built into a slot sized for
the largest (`ui::AllScreens`).

### 7.2 Buttons

| Button | Short press | Long press |
|---|---|---|
| A (top) | Next mode, on the press | Jump to Cruise, screen 0 (after the short) |
| B | Next screen in mode, on the press | — |
| C | Up / previous, on the press | Repeat while held |
| D | Down / next, on the press | Repeat while held |
| E | Accept / select, on release | Context action for the screen |

A button acts the moment its press settles unless it has a long meaning
that cannot follow the short one: waiting for the release cost the length
of the press, 80–150 ms with a gloved thumb, on every screen flip. So B
gave up "previous screen", and long A comes on top of short A, which is
harmless because it goes to Cruise wherever short A went. Only E still
waits, as its short and long are different acts (`core::input::Config`).

The navigation state machine lives in `core/nav/`. It takes an
`InputEvent` and the current `(mode, screen, focus)`, and returns the new
one. It has no hardware in it, so you can unit-test every path today.

```cpp
// core/nav/nav.hpp
struct NavState { Mode mode; uint8_t screen; uint8_t focus; bool menu_open; };
NavState step(NavState s, InputEvent e, const NavLayout& layout);
```

### 7.3 The Screen interface

```cpp
class Screen {
public:
    virtual const char* name() const = 0;
    virtual void enter(Canvas&)            {}   // build widgets
    virtual void leave()                   {}   // free them
    virtual void update(const Frame&)      = 0; // set widget values ONLY
    virtual bool handle(const InputEvent&) { return false; }
    virtual std::span<Widget*> widgets()   = 0;
};
```

`update()` may not draw. It only calls `widget.set(value)`. The render
task then repaints the widgets that changed. This split is why the code
stays readable: a screen file is a list of widgets and a list of
assignments, and nothing else.

### 7.4 Widgets

```cpp
class Widget {
public:
    Rect bounds;
    bool dirty = true;
    virtual void render(Canvas&) = 0;
protected:
    void touch() { dirty = true; }
};
```

The widgets as built (`ui/widgets.hpp`, 23):

`Label` · `ValueBox` · `BigNumber` · `Bar` · `StatusDot` · `ShiftStrip` ·
`DeltaBar` · `LeanIndicator` · `Sparkline` · `Table` · `MapView` ·
`TachBar` · `RevBar` · `LeanGauge` · `MediaRow` · `MediaProgress` ·
`AlbumArt` · `AlertOverlay` · `NavArrow` · `SlipBar` · `HrGraph` · `DevTag` ·
`Toast`

A widget can report the part of itself that changed (`Widget::damage()`),
so `render` sends a table's changed rows, not the table.

`ArcGauge` became `LeanGauge`, which is the only arc anything wanted.
`GBall` went when SPEED dropped it for a bigger lean gauge; its jump face
(airtime, landing g, for a few seconds after a landing) moved into
`LeanGauge`, which shows it in the same place.
`MediaStrip` became three `MediaRow`s, one per line, because a widget is
the unit of redraw and a scrolling album title should not drag a static
artist name down the bus with it.

`AlertOverlay` is mode-independent and draws over whatever screen is up;
so do the `Toast` strip along the bottom and the `SlipBar` across the top.
Which single alert shows is `core/alert`'s decision, with a dismissal on E
that holds until the fault clears.

A widget's `set()` compares to the last value and only calls `touch()` on
a change. That gives you the old dirty-flag win, but in one place instead
of scattered through every screen.

### 7.5 Layout

No layout engine. Each screen file declares `static constexpr Rect`
values at the top. You can read the whole layout of a screen in ten lines.

### 7.6 ILI9488 on an 8-bit 8080 bus

Over 4-wire SPI the ILI9488 does not accept RGB565 and needs RGB666, three
bytes per pixel — the S3 board's problem. On the P4 board the panel is on
an 8-bit 8080 bus, where **RGB565, two bytes per pixel,** works. Use the
`esp_lcd_ili9488` component over an i80 IO handle rather than writing the
command set. Budget: a full 320×480 frame is 307 kB, about 31 ms at the
10 MHz starting write clock and about 20 ms at the panel's ~15 MHz
datasheet ceiling. The first board's panel ran clean at 25 and 30 MHz,
and runs at 25: about 12 ms. Still, draw in bands and send only what
changed. 40-line bands are
the starting point (`bsp::kTftBandLines`). What it became: `render` sends
only damaged rectangles, and `ui::Shadow` keeps a copy of the glass in PSRAM
so a strip goes out only where it differs, with a one-band-a-second scrub
that repairs a panel that lost its memory (`features.md` §4).

---

## 8. The IMU: what you can and cannot get

You asked for lean, pitch, jump detection, and a compass heading.

**Lean.** An accelerometer alone cannot measure lean on a motorcycle. In a
steady turn the bike leans into the resultant force, so the accelerometer
still reads "down" through the bike. It shows near zero lean at 40° of
real lean. You must integrate the roll-rate gyro and correct the drift
with a second reference. The standard reference is:

```
lean ≈ atan( v · ω_yaw / g )
```

where `v` is the road speed and `ω_yaw` the yaw rate. (As built,
`core/attitude` uses the fused speed and the *world* yaw rate Ω through the
Euler-rate transform, with the bar mount and steering angle turned out: the
yaw gyro alone read 33° for a true 40°. `features.md` §14.1.) Feed that as the
slow correction term in a complementary or Mahony filter, and use the
gyro for the fast term. Above about 5 mph this is accurate to a few
degrees. Below 5 mph, fall back to the accelerometer.

**Pitch.** Same filter, other axis. Good for hills and for wheelie angle.
Accelerometer correction works here when the bike is not accelerating
hard, so gate the correction on `|a| ≈ 1 g`.

**Jumps.** Free fall is easy and reliable. Airborne when the vector
magnitude stays under about 0.3 g for more than 100 ms. Airtime is the
time until magnitude passes about 2 g again. Record the landing peak.
This is pure maths on a sample stream, so it goes in `core/jump/` and you
can unit-test it against a recorded CSV.

**Compass heading — not possible.** The ICM-42688-P is a 6-axis part.
It has an accelerometer and a gyro. It has **no magnetometer**. There is
no magnetic sensing element in the package at all, so no software can get
a compass heading from it. Your options:

1. Use GPS course over ground. It is accurate when moving, useless when
   stopped. This costs nothing and is what the map screens should use.
2. Add a magnetometer, for example an MMC5983MA or LIS2MDL, on the free
   I2C bus. On a steel dirtbike frame, near the ignition coil and the
   shift-lamp currents, hard-iron calibration will be poor. Your instinct
   that this is unlikely to work well is right, but the reason is the
   magnetic environment, not the sensor.
3. Integrate the yaw gyro for short-term relative heading. Useful for a
   track map between GPS fixes. It drifts, so never show it as a compass.

**Mount offset.** The board will not sit level in the bike. Add an
`ImuCalibrate` command: park the bike upright, hold E, and store the
gravity vector as the zero reference in NVS. Without this every lean
reading has a fixed bias.

---

## 9. Storage

### 9.1 Three kinds of data

| Data | Where | Why |
|---|---|---|
| Odometer, engine hours | NVS, namespace `life` | Must never be lost |
| User settings | NVS, namespace `cfg` | Small, must survive a card removal |
| Track definitions, lap sessions | SD, `/tracks`, `/sessions` | Large, portable |
| Blackbox and telemetry | SD, `/logs` | Large, high rate |

Keeping `life` and `cfg` in separate namespaces was right in the old
build. Keep it.

### 9.2 The logger

Producers write fixed-size records into a PSRAM ring buffer. The `logger`
task drains the ring to SD in 8 kB blocks. Producers never touch the
filesystem and never block.

`core::log::Record` is 124 bytes fixed; the ring is 512 of them, 62 kB in
PSRAM. Fixed size matters because the drain writes whole records: a
variable-length format means a partial write at power-off corrupts the
tail rather than truncating it.

**The file is CSV, not a binary format.** The original sketch called for
binary — four times smaller, decoded by a script on the PC. That was the
wrong trade. A blackbox you need a tool to read is a blackbox you never
read, and the SD card is 16 GB against a 50 Hz record of 124 bytes, which
is 22 MB an hour. Space is not the constraint.

Given it is CSV anyway, the columns are named for **RaceRender**, so the
same file is both the blackbox and the source for a video overlay
without a conversion step:

```
# RaceRender Data
Time,Latitude,Longitude,MPH,RPM,X,Y,Lap,Heading,Gear,Lean,Pitch,GVert,LapTime,Sats,Flags,FrontMPH,RearMPH,GpsMPH,HDOP,GpsUtc,FusedMPH,FusedSigma,AltFt,HR
```

New columns are appended at the end and never renamed or moved:
RaceRender's overlay templates map by name.

`Time` is in **seconds** — RaceRender reads that column as seconds, and
milliseconds would import a six-minute session as a hundred hours. `X` is
longitudinal g and `Y` lateral, which is its convention. The columns past
`Gear` are ours; it ignores what it does not recognise, and can drive
custom gauges from them.

Completed laps are written as `# Lap 0: 00:02:43.08` tags, from the lap
timer's own measurement rather than left for RaceRender to infer from the
trace. The tag rides along in the record that closed the lap
(`lap_done_ms`) so the logger — the only thing that sees records in order
— can place it between the right two rows. Tags may appear anywhere in
the file but must be in chronological order.

**The rate is a setting** (§13, key `0x0901`), not the tick rate. `logic`
runs at 50 Hz and a decimator in `core::log` decides which of those ticks
gets a row: Off, 1, 5, 10, 25 or 50 Hz, defaulting to 10. The menu offers a
list rather than a number because only divisors of the tick give evenly
spaced rows — 30 Hz would log alternating 20 ms and 40 ms gaps, which
RaceRender reads without complaint and which stops the file being an honest
time base. The rate also sets how much slack the ring has against an SD
stall: 512 records is ten seconds at 50 Hz and a hundred at 5.

One exception, and it is the reason the decimator is not a plain modulo: a
tick that starts or finishes a lap always gets its row, whatever the rate.
The lap tag rides in that record and has no other channel, so decimating it
away would lose the lap from the file. Off is still off — a rate of Off
writes nothing at all, laps included.

Blackbox is the same ring, kept always full. On an event (a hard landing,
a fault, an ignition-off), flush the last N seconds to a separate file.

**Hardware gap, half closed.** The P4 board has a VIN sense divider
(R17/R18 to GPIO20), so firmware can now see the rail falling. It has no
run-on capacitance sized for a flush: at about 0.4 A the bulk on VIN
lasts a few ms, not the ~50 ms a flush needs. Consequences, still:

- You cannot flush the blackbox on power loss. (What *is* done: `power`
  sees VIN fall below 9 V, blanks the backlight and lamps, makes `logger`
  stop starting writes, and saves the odometer and engine hours. See
  bringup §8.1; none of it has run on the board.)
- The FAT filesystem can be left dirty by a sudden cut.

Mitigation now: `core::log::FlushPolicy` puts a block on the card when its
oldest row is 2 s old and fsyncs 5 s after the first uncommitted write or
when the bike stops, so a cut loses up to ~7 s of rows. Fix for the next
revision: enough bulk capacitance on the 3V3 rail for about 50 ms of
run-on, with VIN sense as the trigger.

### 9.3 USB mass storage — a real constraint

The ESP32 and the PC cannot both own the card's filesystem. If both mount
it, you corrupt it. So storage has an explicit owner state machine:

```
        ┌──────────┐  user selects "USB mode"  ┌──────────┐
        │  BIKE    │ ────────────────────────► │  HOST    │
        │ ESP owns │ ◄──────────────────────── │ PC owns  │
        └──────────┘   VBUS gone, or user exit └──────────┘
```

Enter HOST only when: VBUS present **and** RPM is zero **and** no logging
session is open. Unmount cleanly, then start TinyUSB MSC over the same
block device. USB and the bike are diode-OR'd onto VIN (D5, D6), so VBUS
being present does **not** mean the bike is off. Do not use VBUS alone as
the trigger; VIN sense reads about 4.5 V on USB alone and 12–14 V on the
bike, which tells the two apart. Make it a CONFIG mode action, with an automatic
offer when VBUS appears at zero RPM.

---

## 10. Engine and ECU data

```cpp
// hal/i_engine.hpp
class IEngine {
public:
    virtual bool online() const = 0;
    virtual bool read(EngineData& out) = 0;   // false if no data
    virtual ~IEngine() = default;
};
```

Implementations, in the order you will need them:

- `NullEngine` — always offline. Used now. Stat and Diag show `--`.
- `EspNowEngine` — the K-line adapter's frames, adapter C6 → dash C6 over
  ESP-NOW. Not written; see §10.1.

Only one is compiled in at a time. Nothing above `IEngine` changes when
you swap them.

### 10.1 The K-line adapter talks ESP-NOW, not CAN or UART

**The P4 dashboard board has no CAN transceiver**, and the ESP32-P4's TWAI
is logic-level only: it cannot drive a differential pair, and CAN levels on
a 3.3 V GPIO may damage the pin. The board has no spare routed pins either
(`p4-board-pin-map.md` §2.1), so a wired UART link is out too.

The adapter that answers this is `kline-adapter/review.md`: a separate
board with its own ESP32-C6 that sends the ECU's data to the dash's C6
over **ESP-NOW**. It needs no dash pins. It does need work on the dash
side: ESP-Hosted 2.12.8 has no ESP-NOW RPCs, so the C6 firmware
(`demo_p4/c6_cp`) must run ESP-NOW itself and forward frames, and Wi-Fi
must sit on a fixed channel (review §10). The earlier CAN adapter is
`history/kline-can-adapter-netlist-review.md`. The repo records no order
for either board.

Design the frame format now, whichever transport wins. The transport is
20 lines; the decode and the interface are the part worth getting right.

Tach RPM stays separate and always available. It comes from the inductive
pickup on `TACH_SIG` (GPIO21), not from the ECU. Keep it that way even after the
adapter works, so a dead adapter never blanks your RPM.

**Fuel used.** With no flow sensor and no ECU, any fuel figure is a
model, not a measurement. Start with a simple one: fuel rate as a
function of RPM and throttle proxy, integrated over time, with a
correction factor you tune in CONFIG against real fill-ups. Label it as
an estimate on the screen. When the K-line adapter lands and gives
injector duty, replace the model and keep the same interface.

---

## 11. Bluetooth, Wi-Fi and firmware updates

**Moved to `radio-and-updates.md`, with its numbering kept** (§11.1 the
Dash Media GATT contract, §11.1.1 iPhone, §11.2 TPMS, §11.3 radio roles and
pairing, §11.4 core placement, §11.5–11.6 state and screens, §11.7
partitions and stable/dev slots, §11.7.1 Wi-Fi updates, §11.8 failure
rules).

---

## 12. Health and faults

```cpp
enum class DevState : uint8_t { Absent, Init, Ok, Degraded, Fault };

struct DevHealth {
    DevState state;
    uint16_t fault_count;
    uint32_t last_ok_ms;
    int32_t  last_err;
};
```

Every driver owns one `DevHealth`. Rules:

- A driver never calls `abort()`, never blocks forever, and never retries
  in a tight loop.
- The `keeper` task retries a `Fault` device once per 5 s. This turns a
  loose GH1.25 connector into a self-healing problem.
- A widget bound to data from a dead device shows `--`, not a stale value
  and not a zero. Stale zeros on a dashboard are dangerous.
- The DIAG sensors screen shows the whole table. That screen is your main
  bring-up tool when the PCB arrives.

The handlebar pod deserves extra care. It is 0.5 m of I2C on a vibrating
bike. `input_service` must: use a short timeout, count NACKs, and on
repeated failure reset the I2C peripheral and re-init the MCP23017. Also
re-write the configuration registers periodically, because a browned-out
expander comes back with default registers and looks alive but does
nothing.

---

## 13. Configuration

One table drives the whole CONFIG mode. No per-screen field arrays.

```cpp
struct ConfigItem {
    uint16_t    key;            // stable, never reused — NVS key
    const char* label;
    const char* group;          // "Display", "Wheels", "Shift", "Lap"
    Kind        kind;           // Float, Int, Bool, Enum, Action
    float       min, max, step;
    const char* unit;
    void*       target;
};
extern const ConfigItem kConfig[];
```

The CONFIG screen renders the table, groups by `group`, and edits with
C/D/E. Adding a setting is one line in one array. Validation, NVS load,
NVS save, and the diag dump all read the same table.

Keep `key` stable forever. Never renumber. That is what makes an old
NVS partition still load after a firmware change.

---

## 14. Host tests

`test/` is a plain CMake project. It does not use `idf.py`. It compiles
`components/core/**` with g++. It links no test framework: `test/check.hpp`
is a 40-line assertion harness, so there is nothing to install before the
fast loop works. Swap it for Catch2 if you ever want more.

```bash
cmake -S test -B build-host && cmake --build build-host && ctest --test-dir build-host
```

There is one `test/test_*.cpp` per `core/` module, plus `test_ui` (every
screen drawn against a fake Frame, including one with nothing published,
where every reading must come out `--`, and a partial update landing the
same pixels as a full repaint), `test_shadow`/`test_wire` (the bus cost),
and `test_logic_*` (`logic_tick` end to end). `check_no_esp_headers.sh`
enforces rule 3. `test_config` holds the list of shipped CONFIG keys, so a
key can never be renumbered.

This is the strongest reason to keep `core/` free of ESP headers. Half of
your bugs are logic bugs, and you can kill them all this week.

---

## 15. What is different from V1, and why

| V1 | V2 | Reason |
|---|---|---|
| `globals.h`, 60 externs | Publish slots and `Frame` | One writer, no shared mutable state |
| `S` → `g_snap` → `g_*`, two copies | One `snapshot()` read | Half the copying, no mirror layer to keep in step |
| `bulkRev` + 2.4 kB memcpy | `TrackBuffer` read in place | No large copy on the hot path |
| Screens read `g_*` directly | Screens take a `const Frame&` | A screen becomes testable and cannot race |
| `screens.cpp` textually `#include`s `.cpp` | Real CMake components | That trick existed only to fight arduino-cli |
| Field menu per screen | One `ConfigItem` table | Adding a setting is one line, not four files |
| Sprites allocated per screen enter/exit | Widgets own their bounds, shared canvas | No allocation churn, no failure at screen change |
| Encoder mode state machine in the UI | `core/nav/` pure function | Unit-testable, and it survives an input change |
| 8 screens, flat, encoder to cycle | 5 modes × screens, buttons | Matches how you actually ride |

Keep from V1: the dual-core split, the dirty-check before repaint, the
two NVS namespaces, the `portMUX` around ISR state, and the DIAG worst-pass
timing readout. Those were the good parts.

---

## 16. Pin map — the P4 board

`p4-board-pin-map.md` is the full map against the module's 80 pins, and
`bsp/pins.hpp` is the only place these numbers appear in code. The
summary:

| Function | GPIO |
|---|---|
| SPI2 MOSI / SCLK / MISO | 8 / 9 / 10 |
| CS: IMU / SD | 7 / 11 |
| TFT D0–D7 (8080) | 33 down to 26 — reversed as drawn |
| TFT RST / WR / DC / CS | 53 / 52 / 51 / 50 — as drawn |
| TFT TE | 49, optional |
| Backlight | PCA9955B OUT8–10 |
| Shift lamps 1–8 | PCA9955B OUT0–7 |
| PCA9955B SDA / SCL (I2C1) | 34 / 0 |
| GPS UART RX / TX | 2 / 3 |
| IMU data ready (INT1) | 13 |
| Tach input | 21 |
| Wheel hall front / rear | 22 / 23 |
| Neutral input (**active LOW**) | 12 |
| SD card detect (**active LOW**) | 1 |
| VIN sense | 20, ADC1 |
| I2C SDA / SCL (pod) | 4 / 5 |
| USB D− / D+ (USB-Serial/JTAG) | 24 / 25 |
| Boot button / reset | 35 (36 pulled up) / CHIP_PU |
| Debug UART0 TX / RX | 37 / 38, test pads |
| C6 reset / boot | 54 / module pin 18 |

Notes that change firmware:

- **Neutral reads LOW.** The PC817B pulls GPIO12 down through R64 when
  the bike's neutral switch closes.
- **The pod has no interrupt line.** CN1 is SDA, SCL, VCC, GND only, so
  buttons must be polled. The 200 Hz `input` task is required, not a
  choice.
- **Run the pod I2C at 100 kHz.** With 4.7 k pull-ups over 0.5 m of cable
  the rise time is roughly 470 ns. Fast mode allows 300 ns; standard mode
  allows 1000 ns. 400 kHz is out of spec on this harness.
- **Give each SPI device its own clock** in its
  `spi_device_interface_config_t`. The IMU starts at 24 MHz and the SD at
  20 MHz; the bus is about 90 mm long with no source termination, so drop
  toward 10 MHz if either faults.
- **The tach is an inductive wrap on the plug lead, 5–10 V.** The
  as-built R34/C19 filter swallowed its spike; reworked to 10 kΩ / 47 pF,
  `p4-board-pin-map.md` §6.3.

### 16.1 Two hardware limits the firmware must live with

**Power-source sense, but no run-on.** VIN sense tells bike from USB and
sees the ignition go off, so the `Elec` slot has a source and the USB-MSC
handover (§9.3) can use a real test. There is still no run-on
capacitance, so the blackbox cannot flush on power loss (§9.2).

**USB bench current.** On USB, VUSB feeds VIN through D5, so VIN sits at
about 4.3–4.6 V — at the buck's 4.5 V minimum, with the backlight LDO in
dropout. Eight lamps at 30 mA plus the backlight plus the P4 is close to a
500 mA port, so `ctrl` caps lamp duty at `bsp::kShiftDutyUsbMax` whenever
VIN sense says the supply is USB.

---

## 17. Build order

*Status (1.62):* steps 1–4 are done and on the board; 5, 6, 8 (less USB
MSC) and 9 (less TPMS) are written and have partly run; 7 is written and
untested on the bike (no tach signal yet); 10 is not started.

1. `core/` plus `test/`. Nav, lap, geo, units, trip, shift. No hardware.
2. `bsp/` plus `drivers/pca9955b` plus `drivers/ili9488`. Boot to a
   colour bar on the panel.
3. `ui/canvas` plus `ui/widgets`. Draw a fake `Frame` with static numbers.
4. All five modes and their screens, still on the fake frame.
5. `drivers/icm42688p` plus `core/attitude`. Log raw to serial, tune the
   filter against a bench tilt.
6. `drivers/pa1616d` plus `gnss_service`. Then the lap timer against real
   fixes.
7. `ctrl` task: tach, wheels, shift lamps. The latency-critical path last,
   because it is small and you want the diag screens working first.
8. `logger` and `storage`, then USB MSC.
9. `ble`: start NimBLE, advertise, bond. Then TPMS scan, then media. Do
   this after the real-time path works, so that if the shift-light timing
   changes you know the radio caused it.
10. `engine_service` with the CAN driver, when the adapter board is ready.

Steps 1, 3, and 4 need no hardware at all. Start there.

Set the partition table (§11.7) **before** step 2. Changing it later
erases NVS, and NVS holds your odometer.

---

## 18. Open questions

- ~~Backlight: auto-dim from a light sensor?~~ **Built without one:**
  CONFIG → Display → Dim at night scales the brightness setting by the
  sun's elevation, worked out from the GPS date, time and position
  (`core/sun`): full above the horizon, 35 % by six degrees below it.
  It holds its last value through a GPS dropout and is full until the
  first fix. Overcast, tunnels and street lights are not seen.
- ~~Lap delta reference: session best, or saved on the card?~~ **Both**
  (1.30): the quickest clean lap known, the session's or the saved track's
  (`core/track`, `tracks/tNN.trk`). The matcher is still a placeholder.
- Track map: auto-detect the track outline from the first lap, as in V1,
  or load a track file from SD?
- Does the trip map in Cruise mode need any base map, or just a
  breadcrumb trail? A base map means tiles on the SD card and a lot of
  work.
- ~~Which phone do you carry?~~ **Settled: a Sonim XP3900, an Android
  handset**, with the app. An iPhone is served too since 1.59, with no
  app (§11.1.1).
  The dash side is one write characteristic (§11.1); the app is a
  separate piece of work and should wait until the radio works on the
  dash, or you are debugging two unproven ends at once.
- Should Boot dev once and Promote get a confirm step? Each acts on one
  short E; a random-press soak did both (`console.md`).
- Which TPMS sensors? The decoder is trivial once you know the layout, but
  it is guesswork until you have a sensor to sniff. Buy the set before you
  write `core/tpms/`.
- ~~OTA over Wi-Fi through the C6?~~ **Built, §11.7.1:** an on-demand
  hotspot and one page, not a station joining a network.
- Should the media strip show on any screen other than Cruise big-speed?
  Track mode should probably stay clean.
