# Dashboard V2 — System Architecture

Target: ESP32-P4 + ESP32-C6 (Guition JC-ESP32P4-M3-C6 module), ESP-IDF
**v6.0.2**, FreeRTOS, C++17. The pin map is `p4-board-pin-map.md`.

This document was argued for the ESP32-S3 board and carried to the P4
board when the platform changed (`p4-platform-selection.md`). The
architecture did not change; the hardware sections below (§5.1, §6, §7.6,
§11.4, §16) describe the P4 board.

**Status.** This was written as a design, before any of it existed. Most of
it is now built and the two have been reconciled where they drifted, but
read it as the argument for the shape of the thing rather than as a
description of the tree. `CLAUDE.md` holds the current state, and where a
file differs from the sketch in §3 it says why.

Built but never run on the bike board: **BLE media** (§11, step 9 — the
TPMS scan is not written) and **Wi-Fi updates** (§11.7.1). Both have run on
the Guition demo board. Not built, deliberately: **CAN / `engine_service`**
(§10.1 — the board has no transceiver), **USB mass storage** (§9.3 — the
ownership seam exists, the TinyUSB half does not). The P4 board was
ordered on 2026-10-07 and arrived on 2026-10-28; `bringup.md` opens with
what has run on it.

Decisions already made:

| Item | Decision |
|---|---|
| Framework | Pure ESP-IDF. No Arduino layer. |
| Graphics | Own retained-mode widget layer on `esp_lcd`. No LVGL. |
| Data flow | One publish slot per producer. Lock-free seqlock read. |
| Host tests | Yes. All logic builds and runs on the PC. |
| SD card | Config, track logs, blackbox, USB mass storage. |
| Buttons | External I2C pod on the handlebar (MCP23017). |
| Settings | A fifth mode: CONFIG. |
| ECU data | Interface now, null driver now, real driver later. See §10.1 — the dash has no CAN transceiver. |
| IMU | Full fusion: lean, pitch, jump detection. |
| Faults | Never stop. Mark the device dead and keep running. |
| BLE | In. NimBLE. Phone song metadata and TPMS. |
| Wi-Fi | Updates only, on demand. A hotspot the dash runs while parked, never a station; no stored passwords (§11.7.1). |

---

## 1. Design rules

These five rules decide every later argument.

1. **A module gets data from its arguments, not from a global.**
   The old build had `globals.h` with 60 `extern` variables. That is gone.
2. **One writer per piece of state.** No mutex is needed if only one task
   writes a value. Readers get a consistent copy with a sequence counter.
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
services/ │ tasks: sensor_hub, gnss, imu, engine,    │
          │ input, ble, logger, storage, health      │
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
│   │   ├── state/               # Published<T>, VehicleState, Frame
│   │   ├── attitude/            # lean, pitch, yaw rate filter
│   │   ├── jump/                # airtime and landing detect
│   │   ├── lap/                 # lap timer state machine, delta
│   │   ├── trip/                # odometer, tripmeter, fuel model
│   │   ├── shift/               # RPM to shift-light pattern
│   │   ├── geo/                 # distance, bearing, line crossing
│   │   ├── units/               # mph, km/h, °C, °F, one place
│   │   ├── nav/                 # mode and screen state machine
│   │   ├── tpms/                # advertisement decoders, one per family
│   │   └── config/              # ConfigItem table, validation
│   ├── services/               # one file per task
│   │   ├── ctrl_service         # tach, wheels, shift output  (was sensor_hub)
│   │   ├── gnss_service  imu_service  input_service
│   │   ├── logic_service        # thin; the work is in logic_tick.cpp, which
│   │   │                        #   the simulator calls too
│   │   ├── logger_service       # ring buffer to SD
│   │   ├── storage_service      # NVS + SD files + USB MSC owner
│   │   ├── keeper_service       # health retry, NVS writes, backlight
│   │   ├── queues.cpp           # the command and input queues
│   │   ├── ble_service.cpp      # Dash Media GATT over the C6; no TPMS yet
│   │   └── engine_service/      # NOT BUILT — no CAN transceiver (§10.1)
│   └── ui/
│       ├── canvas.hpp  framebuffer_canvas.cpp   # primitives, clipping
│       ├── widgets.hpp/.cpp     # all 16 widgets in one pair
│       ├── screens_cruise.cpp  screens_track.cpp  screens_stat.cpp
│       ├── screens_diag.cpp    screen_config.cpp
│       ├── geometry.hpp theme.hpp chrome.hpp    # palette, meaning, furniture
│       ├── font5x7.cpp  icons.cpp
│       └── app.cpp              # the render loop and mode/screen dispatch
└── test/                        # host build: plain cmake + ctest
    ├── CMakeLists.txt           # plain cmake, does not use idf.py
    └── test_lap.cpp  test_attitude.cpp  test_nav.cpp  ...
```

`core/` and `test/` are the parts you can write today, before the PCB lands.

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

`core/tpms/` does not exist yet — it arrives with the radio.

---

## 4. The state model

### 4.1 Publish slots

Each producer owns one slot. One writer, many readers, no mutex.

```cpp
// core/state/published.hpp
template <typename T>
class Published {
public:
    void publish(const T& v) {              // writer task only
        seq_.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        data_ = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_relaxed);
    }

    T get() const {                          // any reader
        for (;;) {
            const uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1u) continue;            // a write is in progress
            T out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == a) return out;
        }
    }
private:
    std::atomic<uint32_t> seq_{0};
    T data_{};
};
```

This is about 20 lines. It replaces the old `g_snapMutex`,
`publishSnapshot()`, `copySnapshotToGlobals()`, and every `g_*` mirror.

Two things about the reader loop that are easy to get wrong, and that a
stress test caught in the first draft of this document:

- **The retry must be a `for(;;)`, not a `do/while(a != b)`.** In a
  do-while, `continue` jumps to the condition, so the odd-sequence bail-out
  compares against an uninitialised `b` and can return a torn value. That
  version passed casual use and failed at about one read in a million under
  three concurrent readers.
- **A reader copying a torn value is expected and fine.** The second
  sequence load is what detects it, and the copy is discarded. Do not
  "optimise" that load away.

### 4.2 The slots

```cpp
// core/state/vehicle.hpp
struct Engine   { float rpm; bool neutral;
                  float tps_pct; bool ecu_online; };
struct Motion   { float gps_mph, front_mph, rear_mph, best_mph;
                  float accel_g; };
struct Attitude { float lean_deg, pitch_deg, yaw_rate_dps;
                  float g_lat, g_lon, g_vert; bool airborne;
                  uint32_t airtime_ms; float last_land_g; };
struct Gnss     { bool fix; uint8_t sats, quality; double lat, lon;
                  float alt_m, hdop, course_deg, speed_mph;
                  uint32_t utc_ms; };
struct Trip     { float odo_mi, trip_a_mi, trip_b_mi;
                  float fuel_used_gal, eng_hours; float top_mph; };
struct Lap      { LapPhase phase; uint16_t count;
                  uint32_t current_ms, last_ms, best_ms;
                  int32_t delta_ms; float dist_to_line_ft; };
struct Elec     { float vbat, v5, v3v3; };
struct Media    { char title[60], album[60], artist[60];
                  uint8_t state, src; uint32_t updated_ms; };  // see §11.5
struct TpmsWheel{ float psi, temp_c; uint8_t batt_pct;
                  bool alarm; uint32_t age_ms; bool valid; };
struct Tpms     { TpmsWheel front, rear; };
struct Health   { DevState imu, gnss, led_driver, mcp_pod, sd, ecu,
                            display, ble_host, phone, tpms_f, tpms_r; };

struct Slots {
    Published<Engine>   engine;
    Published<Motion>   motion;
    Published<Attitude> attitude;
    Published<Gnss>     gnss;
    Published<Trip>     trip;
    Published<Lap>      lap;
    Published<Elec>     elec;
    Published<Media>    media;      // written by ble only
    Published<Tpms>     tpms;       // written by ble only
    Published<Health>   health;
};
```

Note the split. `Attitude` is written only by the IMU task. `Gnss` is
written only by the GPS task. Nothing needs a lock.

### 4.3 The Frame

The render task reads every slot **once** per frame, into one struct.

```cpp
struct Frame {
    Engine engine; Motion motion; Attitude att; Gnss gnss;
    Trip trip; Lap lap; Elec elec; Health health;
    uint32_t t_ms;
};
Frame snapshot(const Slots&);   // reads all slots, ~200 bytes
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

```cpp
struct Command {
    enum class Id { LapSetLine, LapArm, LapClear, TripReset,
                    ConfigSet, LogStart, LogStop, StorageToHost,
                    ImuCalibrate, ImuSetMountOffset } id;
    union { float f; int32_t i; struct { uint16_t key; float val; } cfg; };
};
QueueHandle_t cmd_q;    // drained by the logic side of sensor_hub
```

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
| `input` | 0 | 9 | 200 Hz | I2C pod poll, debounce, event queue, pod bus recovery |
| `render` | 1 | 6 | 120 Hz | frame snapshot, widgets, 8080 panel bus |
| `logger` | 1 | 2 | drain | ring buffer to SD (below the rest: IDF's SD driver spins through card busy time) |
| `storage` | 1 | 3 | — | NVS, card mount, the USB-MSC ownership seam |
| `keeper` | 1 | 3 | 1 Hz | NVS save (only when `core::flash::still`), health retry policy (the owning task re-inits), backlight level |
| `ble` | 1 | 4 | event | media Feed and Ctrl characteristics (§11); TPMS scan not written |
| `ota` | 1 | 2 | event | the stable/dev slots, the only writer of the dev slot (§11.7) |
| `wifi` | 1 | 3 | 4 Hz | the update hotspot and its web server, off until asked (§11.7.1) |

The GPS is polled at 20 ms rather than reading a UART event queue. That
was measured and left alone: 10 ms on a path whose end-to-end latency is
140 ms, which is nothing a rider can see.

The IMU is not polled. It blocks on INT1 with a timeout, and the timeout
is a fallback rather than the design — see §8. PPS is not wired on the P4
board; `bsp/pins.hpp` says why.

Twelve tasks of your own. Each one has a single job you can say in one line.
Stack sizes and high-water marks go on the DIAG system screen.

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
- **I2C1** — PCA9955B only, 1 MHz: shift lamps from `ctrl`, backlight from
  `keeper`. A crushed pod cable cannot take the lamps with it.
- **UART1** — GPS only. Owned by `gnss`.

### 5.2 Latency budget

Tach edge to shift lamp must stay under about 3 ms, as before. Path:
GPIO edge → `ctrl` at 500 Hz (2 ms worst) → one PCA9955B I2C write
(~100 µs at 1 MHz). `ctrl` runs at priority 18, above everything. The render task
cannot delay it, because it is on the other core and lower priority.

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
| **Cruise** | Big speed · Trip stats · Trip map |
| **Track** | Lap timer (time, speed, delta) · Lap history · Track map |
| **Stat** | All data (incl. TPMS) · Engine data · IMU / attitude |
| **Diag** | Sensors · GPS · System (ESP32) · Bus and I/O · BLE |
| **Config** | Generated list from the `ConfigItem` table |

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

Set of widgets to start with:

`Label` · `ValueBox` · `BigNumber` · `Bar` · `Sparkline` · `Table` ·
`MapView` · `StatusDot` · `DeltaBar` · `LeanIndicator` · `LeanGauge` ·
`ShiftStrip` · `TachBar` · `MediaRow` · `AlertOverlay`

`ArcGauge` became `LeanGauge`, which is the only arc anything wanted.
`GBall` went when SPEED dropped it for a bigger lean gauge; its jump face
(airtime, landing g, for a few seconds after a landing) moved into
`LeanGauge`, which shows it in the same place.
`MediaStrip` became three `MediaRow`s, one per line, because a widget is
the unit of redraw and a scrolling album title should not drag a static
artist name down the bus with it.

`AlertOverlay` is mode-independent. A TPMS low-pressure warning draws over
whatever screen is up. It is the only thing allowed to break the
one-screen-at-a-time rule, and it needs a dismiss action on button E.

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
the starting point (`bsp::kTftBandLines`).

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

where `v` is GPS speed and `ω_yaw` is the yaw-rate gyro. Feed that as the
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

`core::log::Record` is 72 bytes fixed; the ring is 512 of them, 36 kB in
PSRAM. Fixed size matters because the drain writes whole records: a
variable-length format means a partial write at power-off corrupts the
tail rather than truncating it.

**The file is CSV, not a binary format.** The original sketch called for
binary — four times smaller, decoded by a script on the PC. That was the
wrong trade. A blackbox you need a tool to read is a blackbox you never
read, and the SD card is 16 GB against a 50 Hz record of 72 bytes, which
is 13 MB an hour. Space is not the constraint.

Given it is CSV anyway, the columns are named for **RaceRender**, so the
same file is both the blackbox and the source for a video overlay
without a conversion step:

```
# RaceRender Data
Time,Latitude,Longitude,MPH,RPM,X,Y,Lap,Heading,Gear,Lean,Pitch,GVert,LapTime,Sats,Flags
```

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

Mitigation now: pre-allocate log files, call `fsync` every second, and
never leave a file handle open across a long idle. Fix for the next
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
- `UartEngine` — framed serial from the adapter. See §10.1.
- `CanEngine` — TWAI driver. Needs a transceiver the dash does not have.
- `BleEngine` — the adapter as another BLE peripheral. No wires.

Only one is compiled in at a time. Nothing above `IEngine` changes when
you swap them.

### 10.1 The dash cannot receive CAN

**The P4 dashboard board has no CAN transceiver**, and neither did the
S3 board before it. Its part list has no SN65HVD230, no TJA1050, no
MCP2551. The ESP32-P4 has TWAI controllers, but TWAI is logic-level TX/RX
only. It cannot drive a
differential pair, and CAN levels on a 3.3 V GPIO may damage the pin.

So the K-line adapter, as built, has no one to talk to until dash v2.

The adapter's own `CANH`/`CANL` nets run through R5 and the SN65HVD230
before they reach the output block, so you cannot repurpose those two
wires as a UART without cutting the adapter board. A serial link means an
adapter respin. Options, ranked by what they cost you today:

| Option | Cost | When |
|---|---|---|
| Stay on `NullEngine` | Nothing | Now. The screens already handle it. |
| `BleEngine` | Firmware both ends | Now, if you accept the adapter using its radio |
| `UartEngine` | Adapter respin, 2 dash spare pins | Next adapter revision |
| `CanEngine` | Transceiver on dash v2 | Correct end state |

The P4 board leaves module pins unconnected (`p4-board-pin-map.md`
§2.1), which is enough for a UART pair or for TWAI TX/RX into a dead-bug
transceiver if you want to prove the path before a respin.

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

## 11. Bluetooth (BLE)

Two features: now-playing song metadata from the phone, and TPMS sensor
readings. Both use BLE only. Both are optional at runtime — if the radio
never connects, nothing else in the dash changes behaviour.

### 11.1 The hard constraint: no Bluetooth Classic

The radio is the module's ESP32-C6, which has **BLE 5 only**, like the
S3 before it. It has no Bluetooth Classic radio at all. That rules out AVRCP, which is the normal way a car stereo gets track
metadata, and it rules out SPP, which is the normal way to get a serial
stream. There is no workaround in firmware.

The phone is a Sonim XP3900, an Android handset, so there is one path:
something runs on the phone and pushes strings to the dash. The V1 build
worked this way too.

**BLE offers exactly two connection-oriented transports**: ATT/GATT, and
L2CAP connection-oriented channels. There is no third. Advertising cannot
carry this — 31 bytes of payload will not hold a song title, and it is
fire-and-forget with no delivery guarantee.

We take GATT, in its smallest form: **one characteristic that the phone
writes bytes to.** L2CAP CoC is a real socket and would also work, but
nRF Connect and every other off-the-shelf BLE app speak GATT and not CoC.
When neither the dash nor the phone app has ever worked, being able to
poke the dash from an app you did not write is worth more than the
difference between `write()` and a stream.

```
Service   6e5f0001-b5a3-f393-e0a9-e50e24dcca9e   "Dash Media"
  char    ...0002  Feed     write-no-response, UTF-8
```

One handle, one line per write, tag character then the value:

```
T The Devil In I
A Slipknot
L .5: The Gray Chapter (Special Edition)
S 1
```

| Tag | Meaning | Value |
|---|---|---|
| `T` | Title | UTF-8, truncated to 59 bytes on arrival |
| `A` | Artist | as above |
| `L` | aLbum | as above (`A` was taken) |
| `Y` | lYric | current line only, as above (`L` was taken) |
| `S` | State | `0` stop, `1` play, `2` pause |
| `D` | Duration | track length, decimal ASCII milliseconds; `0`/absent is unknown |
| `P` | Position | decimal ASCII milliseconds, **an anchor, not a live value** — see below |
| `N` | Notification | `N<kind> <who>` 0x1F `<text>`, one write — see below |

Rules the firmware side depends on:

- **One line per write.** The trailing newline is optional; the end of the
  write is the end of the line. No reassembly across writes, so a dropped
  write costs one field and not the parser's state.
- **No chunking.** `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` is 256, so a
  60-byte string is a single write. If a negotiated MTU ever comes back
  smaller, truncate on the phone rather than chunking here.
- **Any tag may arrive at any time**, and unknown tags are ignored rather
  than being an error. That is what lets a tag be added later without
  breaking a phone app that predates it.
- **`updated_ms` is set on every accepted line.** Screens use it as the
  "is there any media" gate.
- **On disconnect, `ble` must publish a zeroed `Media`** — after a
  ten-second hold (§11.8, `core::media::kDisconnectHoldMs`), so a link that
  blinks out and straight back keeps its title. A dash still showing what
  was playing forty minutes ago is worse than one showing nothing, and
  nothing downstream can tell stale from current.
- **`P` is not resent every second of playback.** A `MediaController`
  position callback fires continuously on the phone, and turning that into
  a BLE write a second would be most of the link's traffic for a number
  the dash can work out itself. `P` instead means "position was this many
  ms, as of now" — the dash records its own clock alongside it
  (`Media.position_at_ms`) and, while `S` says playing, adds the elapsed
  wall time on top for display. The phone resends `P` on connect, on a
  track or seek change, and otherwise only as an occasional resync beat
  (alongside `S`'s existing one) to correct for drift — never on every
  position tick.
- **`Y` is the opposite of `T`/`A`/`L`: it changes continuously during
  playback, once per lyric line, and is never resent as a liveness beat.**
  The phone reads the `.lrc` sitting next to the track (same file the
  Lyrics screen already parses) and writes the line whose timestamp the
  playhead has just reached — nothing is chunked or reassembled, so only
  one line is ever in flight, never a whole file. The dash never stores a
  lyric file: it holds exactly the line it was last told, same as any
  other `Media` field, and a title change clears it immediately rather
  than leaving the old track's last line on screen under a new song.

- **`N` is a notification, not media** (1.32). One write carries a whole
  one: a kind byte, the caller/sender/app (31 bytes), ASCII's unit
  separator 0x1F, and the message (95 bytes, absent for a call). Kinds:
  `C` an incoming call, ringing; `E` that call ended (answered, rejected,
  gone), which changes nothing but the ringing; `X` missed call; `M`
  message; `O` other. It lands in its own `Notify` slot (`ble` its one
  writer, parsed by `core::media::apply_notify`), so a new song does not
  clear it and a disconnect only stops a call ringing; a call also stops
  after 45 s with no `E`. The phone sends calls, missed calls and messages
  only (`Notify.kt`'s `NotifyFilter`), and sanitises both fields so 0x1F
  can only be the separator. The dash shows it as a toast and on CRUISE →
  PHONE, with the message text only when stopped unless CONFIG → Phone →
  Text moving is on.

`Media.src` records which path fed the data: `kSrcPhone` for the feed,
`kSrcAms` for an iPhone (§11.1.1).

### 11.1.1 An iPhone: AMS and ANCS, no app (1.59)

An iPhone serves two GATT services of its own to any bonded accessory:
**AMS** (Apple Media Service, `89D3502B-0F36-433A-8EF4-C502AD55F8DC`) for
what is playing and the transport keys, and **ANCS** (Apple Notification
Center Service, `7905F431-B5CE-4E99-A40F-4B1E122D00D0`) for calls and
messages. So an iPhone needs no app: the dash reads both as a GATT
*client* over the phone's own connection. It stays the peripheral; the
iPhone connects to it. The GATT client was already on for the watch (1.48).

Not a setting. The two paths share the connection, the bond and every rule
of §11.3, and which one a phone uses is found out, not configured:

1. The phone connects and encrypts, and `core::ble::judge` lets it in, as
   for any phone.
2. The `ble` task (`AppleLink`) looks through the phone's services. No AMS
   or ANCS: an Android phone, which writes Feed. Otherwise it subscribes:
   AMS's Entity Update (track title, artist, album, duration; then the
   player's playback info), then ANCS's Data Source and Notification Source,
   in that order, as Apple asks.
3. `core/media/apple` turns each AMS update into the §11.1 line it amounts
   to (`T`, `A`, `L`, `D`, `S`+`P`) and each ANCS notification into an `N`
   line, and hands them to the feed's own `apply_line()`/`apply_notify()`.
   The bounds, the UTF-8 cuts and the "new title restarts the clock" rule
   are the feed's, not a second copy.
4. A handlebar press goes to AMS's Remote Command (toggle, next, previous)
   when the phone has AMS, and over Ctrl otherwise.

ANCS says only that a notification exists. Its title and text are asked for
on the Control Point, one at a time, in lengths `Notify` can hold, and the
answer comes back on the Data Source, possibly in pieces;
`core::media::ancs::Tracker` queues the asks and puts the answers together.
The same three kinds as the Android app: an incoming call (category 1),
rings until iOS removes it, a missed call (2), and a message (Social, 4).
Everything iOS replays on subscribing is skipped, except a call ringing now.

**Advertising.** The scan response solicits ANCS (AD type 0x15). That is
what makes an iPhone list the dash under Settings → Bluetooth, and pairing
from there is how it bonds. The 128-bit solicitation fills 18 of the 31
bytes, so the scan response carries the shortened name `CRF250F`; the full
name is in the GAP service. The advertisement itself is unchanged, so the
Android app's service-UUID scan still finds it.

DIAG → Radio's Ctrl row shows `AMS + ANCS` for an iPhone that is live.

### 11.2 TPMS

BLE TPMS sensors are almost all **advertising-only beacons**. They never
connect. They put pressure, temperature, battery, and an ID into the
manufacturer-specific data field of an advertisement, every few seconds
while the wheel is rolling. So the dash only needs a passive scanner.

That is good news architecturally: no connection state, no pairing, no
reconnect logic. It is a decode problem, not a Bluetooth problem.

```cpp
// core/tpms/decode.hpp   — pure, host-testable
struct TpmsReading { uint32_t id; float psi; float temp_c;
                     uint8_t batt_pct; bool alarm; };

using Decoder = bool (*)(const uint8_t* mfg, size_t len, TpmsReading& out);
extern const Decoder kDecoders[];   // one per sensor family
```

Put the decoders in `core/`, and unit-test each one against a captured hex
string. When you buy a set of sensors and they turn out to use a layout
you have not seen, you add one function and one test, and you never touch
the BLE code.

Bind a sensor ID to a wheel position in CONFIG — a "learn front" and
"learn rear" action that stores the next new ID it sees. Do not guess by
signal strength.

**Worth a sanity check before you buy:** most cheap BLE TPMS sensors are
valve-cap type and add rotating mass at the rim, and they only transmit
above a speed or pressure-change threshold. On a tubed dirtbike wheel
running low pressure, check that the sensor's range actually covers your
pressures before you commit.

### 11.3 Radio topology

The dash needs these BLE roles at once:

- **Peripheral** — advertises the Dash Media service; the phone connects
  in and writes to it.
- **Observer** — passive scan for TPMS advertisements.
- **Central and GATT client** (1.48) — the rider's watch. With CONFIG →
  Heart rate → Watch HR on, `ble` scans for the standard Heart Rate Service
  (0x180D), connects to the strongest advertiser above -85 dBm (then only
  to that one, by address or name, for the power-on), and subscribes to
  Heart Rate Measurement (0x2A37). A Garmin does this with Broadcast Heart
  Rate on. `core/hr` is the decision side and is host-tested. The watch is
  the second of `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`; its connection has its
  own GAP callback, so the phone's pairing policy never sees it, and its bond
  is never stored (the store is the phones', §11.3 below). The reading goes
  to the `heart` slot, STAT → HEART, DIAG → Radio, the blackbox's `HR`
  column and a `heart` event in the detail log.

  The search is continuous while the setting is on and no watch is linked
  (1.52): 3 s scans back to back, each listening 50 ms in 100, so a
  broadcast turned on after power-on is found within a scan or two. The scan
  asks the controller **not** to filter duplicates: the C6 image's filter is
  by address and never refreshed (`CONFIG_BT_LE_SCAN_DUPL_CACHE_REFRESH_PERIOD=0`),
  so a watch heard advertising for its phone before Broadcast HR was on was
  never reported again. `hr_disc_event` passes each Heart Rate advertiser on
  once a scan, and once more with its name. A connect gets 4 s.

Central and the GATT client were off before 1.48 (they had been for AMS,
§11.1) to save flash. Turning them on, with the watch code, cost 31 kB of
image and under 0.5 kB of internal `.bss`. The observer role came with
them; the TPMS scan will use it.

**IDF v6.0.2 bug this exposed.** With `CONFIG_BT_NIMBLE_USE_ESP_TIMER=n`,
NimBLE's FreeRTOS callouts never learn their event queue, so every NimBLE
timer ran on FreeRTOS's timer task, not the host task. With the central
role on, the host timer fires during the controller sync and asserts
(`ble_hs_timer_exp`, ble_hs.c:570), a boot loop. `ble_service.cpp` mends it
by wrapping `npl_freertos_funcs_init` at link time (`-Wl,--wrap`, in
`components/services` and `demo_p4/main`).

NimBLE supports this. The cost is radio time. Scanning and an active
connection compete for the same antenna, so duty-cycle the scan:

| Setting | Value | Why |
|---|---|---|
| Scan interval / window | 300 ms / 30 ms | 10% duty. TPMS beacons repeat, so you catch them within a few seconds. |
| Connection interval | 100–200 ms | Metadata is not latency-critical. Long intervals save radio time and phone battery. |
| Max connections | 1 | One phone. `ble` tracks a single connection for Ctrl, and advertising stops while it is connected. |
| Bonding | Required, stored in NVS | The bond is the phone's identity; see below. |

**Which phone gets in.** One connection means a stranger's phone holding
it is a dash with no media, and Android rotates its address every fifteen
minutes, so the dash cannot recognise a phone by address. Only the
identity key that bonding exchanges does that. So the dash asks for
encryption the moment a phone connects (`ble_gap_security_initiate`), Feed
needs an encrypted link (`BLE_GATT_CHR_F_WRITE_ENC`), and the policy in
`core/ble/pairing.hpp` decides who stays:

- The first phone ever bonds freely — a new dash, or one whose bonds were
  just forgotten, must be reachable without a menu.
- After that a new phone bonds only inside CONFIG → Phone → **Pairing mode**,
  a 60 s window that admits one phone. Opening it lets the current phone
  go, and bonded phones are held off (not forgotten) until it closes, or
  the rider's own phone would reconnect into the slot first.
- A bond made outside a window is deleted and the link dropped. NimBLE may
  only evict an old bond to make room while a window is open, so repeated
  attempts cannot push the rider's phone out of the store.
- A link that has not encrypted within 30 s is dropped.
- **Forget phone** clears every bond. The phone keeps its half until the
  rider forgets "CRF250F-Dash" in Android's Bluetooth settings too; until
  then its encryption fails and the app says so.

A Just Works bond has no MITM protection, and this is not trying to be
security: it stops the dash's one slot going to whoever connects first.
nRF Connect still works for bring-up; it bonds when asked.

### 11.4 Task and core placement

**This is the part that can break your shift lights.** On the S3 the BLE
controller task ran at FreeRTOS priority 23 on the same chip as `ctrl`
(priority 18). On the P4 board the controller runs on the ESP32-C6, so
that task is off the P4 entirely. What stays on the P4 is the NimBLE
host and the ESP-Hosted SDIO transport, and both must stay off core 0.

Pin the NimBLE host to **core 1**:

```
CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y
```

The ESP-Hosted transport is the catch. Its SDIO and RPC tasks
(`sdio_read`, `sdio_write`, `rpc_rx`, …) are created inside the component
at priority 23 with **no core affinity**, and there is no Kconfig for
either. Left alone, they can run on core 0 above `ctrl`. `ble` lowers each
one by name to priority 8 once the link is up: below everything on core 0,
so any real-time task preempts them at once, and above `render`, so the
radio still beats the panel on core 1. It logs how many it found, which is
how a future esp_hosted that renames them would show up. `demo_p4/` is the working reference for the transport, but it
leaves NimBLE on core 0 — fine on a demo with no real-time path, wrong
here. Do not copy its `sdkconfig`.

Core 0 stays a clean real-time island: `ctrl`, `imu`, `gnss`, `logic`,
`input`. Core 1 absorbs the radio alongside `render`, `logger`, `keeper`.
The render task will be preempted sometimes and drop a frame. At 120 Hz
with an 8.3 ms period, that is invisible.

Your V1 build already pinned NimBLE to core 1 for exactly this reason.
That decision was correct. Keep it.

Add one application task:

| Task | Core | Prio | Rate | Owns |
|---|---|---|---|---|
| `ble` | 1 | 4 | event | media GATT server, TPMS scan callback, publishes `Media` and `Tpms` |

The task itself does almost nothing. It receives NimBLE callbacks, runs a
decoder, and publishes. All the real work is in the NimBLE host task,
which the IDF creates for you.

### 11.5 State

```cpp
struct Media { char title[60]; char album[60]; char artist[60];
               char lyric[60];       // tag `Y`, current line only
               uint8_t state;        // 0 stop 1 play 2 pause
               uint8_t src;          // 0 none 1 phone app
               uint32_t updated_ms; };   // 0 means no media at all

struct TpmsWheel { float psi, temp_c; uint8_t batt_pct;
                   bool alarm; uint32_t age_ms; bool valid; };
struct Tpms { TpmsWheel front, rear; };
```

Both go in `Slots` as their own `Published<T>`, written only by `ble`.
`Media` is 188 bytes, so the seqlock copy is cheap enough at 60 Hz.

Add to `Health`: `ble_host`, `phone`, `tpms_f`, `tpms_r`. A TPMS wheel is
`Degraded` when its reading is older than about 5 minutes while moving,
and `Absent` when no ID is bound.

### 11.6 Where it shows on screen

- **Cruise → big speed.** Three `MediaRow` widgets in the bottom right:
  song, album and artist, each behind its icon, scrolling if the name is
  wider than the row. The whole block disappears when `updated_ms` is 0,
  so it never steals space from the speed digits. **Built already** —
  the rows are laid out and drawing, and the only missing piece is the
  radio that fills the slot.
- **Cruise → MEDIA.** The dedicated media screen: song, album, artist and
  a fourth `MediaRow` for the current lyric line (tag `Y`), plus an
  `AlbumArt` widget between the speed tile and the progress ring. Art is
  matched locally against `ui/album_art.hpp` — a handful of albums baked
  into flash at compile time — by the album title already on the wire;
  nothing about a cover ever crosses BLE. Blank when the album is not one
  of the baked-in ones, which is the common case for anyone's whole
  library.
- **Stat → all data.** TPMS front and rear pressure and temperature.
- **Diag → a new BLE screen.** Connection state, bonded device, RSSI,
  last line accepted on the Feed characteristic, last TPMS advertisement
  age per wheel, decoder hit and miss counts. This screen is what you will actually use to work out
  why a sensor is not showing up.
- **Config.** Forget phone and pairing mode (built, §11.3); learn front
  sensor, learn rear sensor, TPMS low-pressure alarm thresholds (with TPMS).

A TPMS low-pressure alarm should be a mode-independent overlay, not a
screen. Losing air is worth interrupting the trip map for.

### 11.7 Partition table, firmware slots and updates

The table is `partitions.csv`, and it must be final before the first flash:
moving NVS later erases it, and NVS holds the odometer.

The table itself is at 0x10000 (`CONFIG_PARTITION_TABLE_OFFSET`), not IDF's
0x8000. The bootloader at 0x2000 runs up to the table, and nothing updates
it over the air, so the bike keeps the bootloader it is first flashed with;
at 0x8000 that bootloader had 1.1 kB to spare, at 0x10000 it has 33 kB.
Moved on 2026-10-17, before the bike's first flash. Both `main.cpp`s refuse
to build with any other offset, since a bootloader looking for the table in
the wrong place does not boot.

```
# name     type  subtype   offset     size
nvs        data  nvs       0x11000    0x10000    # settings, odometer, BLE bonds
otadata    data  ota       0x21000    0x2000     # which app slot boots
coredump   data  coredump  0x23000    0x1D000
ota_0      app   ota_0     0x40000    0x600000   # 6 MB
ota_1      app   ota_1     0x640000   0x600000   # 6 MB
slave_fw   data  0x40      0xC40000   0x200000   # the C6's firmware
# 0xE40000-0x1000000 (1.75 MB) unallocated: room for an assets partition
```

Secure boot and flash encryption are off by decision: both burn eFuses for
good, and either would end USB recovery, `tools/flash_dev.py` and Wi-Fi
uploads to the dev slot. `sdkconfig.defaults` says so and `main.cpp`
refuses to build with either on.

`dash.bin` is about 1.8 MB (2.0 MB on the demo, as of 1.40), so a slot is
under a third full.

**Stable and dev.** The two slots hold a stable image and a dev image. Which
is which is never stored: it is read off the bootloader's own state for the
running slot (`core/ota/`, host-tested), because that is the one record a
crash cannot leave out of date. It rests on
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, which `main.cpp` refuses to build
without, and which has to be in the first image flashed: the bootloader is
not updated over the air.

- **Stable** is whatever runs with state Valid, or with no state at all.
  `idf.py flash` erases otadata, so a USB flash is always the new stable:
  USB is the recovery path.
- **Dev** is the other slot. "Boot dev once" sets it to boot and restarts.
  The bootloader marks it pending, and a dev image never confirms itself,
  so the next reset of any kind (crash, watchdog, ignition) marks it
  aborted and boots stable. A dev build that hangs costs one key cycle.
- **Promote** is always the rider's decision, never a timer. While dev is
  running, DIAG → Update → Promote to stable confirms it on the spot
  (`esp_ota_mark_app_valid_cancel_rollback`), with no restart; from then on
  it is stable and the old stable's slot is where the next install goes.
  Not promoted, it is gone at the next reset.
- Dev cannot install anything: IDF refuses `esp_ota_begin` while the running
  image is pending. Installs run from stable and only ever write the dev
  slot, so **stable is never overwritten while it is the fallback.**

**Every power-on boots stable, with nothing to wait for.** There is no
boot-time chooser: the dash comes up as fast as the hardware allows, and
dev is reached only from DIAG → Update → Boot dev once. A dev image wears a
red DEV tag in the top-right corner of every screen.

**Getting an image into the dev slot**, from DIAG → Update:

| Path | How |
|---|---|
| SD card | `update/dash.bin` on the card → Install from SD. Header checked first (chip, project, size), then written a sector at a time, then the image's own hash checked by `esp_ota_end`. |
| USB | `tools/flash_dev.py` reads otadata, works out stable as the bootloader would, and writes the other slot without touching otadata. (`idf.py flash` is the other USB path: it makes a new stable.) |
| Wi-Fi | DIAG → Update → Wi-Fi, then upload `dash-*.bin` from the dash's page. Same header check, same writer, same parked rule as the card; see §11.7.1. |

Every write is refused unless parked (not moving, engine off): an erase
stops the flash cache on both cores, long enough to break the 3 ms
tach-to-lamp budget.

**Versions and file names.** `firmware.txt` at the repo root holds the
version and, in one to three words, what is new. `cmake/dash_version.cmake`
makes the image's version string `"<version> <board> <feature>"`, for
example `1.0 demo Wi-Fi OTA` (31 characters at most, checked at configure
time), with `bike` from the root project and `demo` from `demo_p4/`. Every
build copies its image to `releases/dash-<version>-<date>-<board>-<feature>.bin`,
named from what is inside it, and `releases/` is kept in git. The build date
is the image's own `esp_app_desc_t` date, kept current by a daily stamp
(IDF otherwise only refreshes it when the version string changes). The
Update screen and the Wi-Fi page show version and date for both slots, and
firmware downloaded from the page is saved under the same name.
`core::ota::check_board` refuses an image whose board word differs from the
running one's; an image from before this scheme has no board word and is not
refused.

### 11.7.1 Wi-Fi updates

The dash is a **hotspot, never a station.** It works at a track with no
router, it stores no network passwords, and nothing on the dash ever
connects out. The C6 runs the soft AP through `esp_wifi_remote` over the
same SDIO link as BLE; the P4 runs lwIP and `esp_http_server`. The C6 must
carry the BLE + Wi-Fi build (`CONFIG_ESP_HOSTED_CP_WIFI=y`, about 1.3 MB of
`slave_fw`'s 2 MB). That build has the same ESP-Hosted version as the
BLE-only one, so the version check cannot tell them apart: a C6 that answers
`esp_wifi_init` with `ESP_ERR_NOT_SUPPORTED` gets its firmware replaced from
`slave_fw` and the dash restarts.

| | |
|---|---|
| On | DIAG → Update → Wi-Fi, or CONFIG → Wi-Fi. A toggle. |
| Network | `CRF250F-Dash`, WPA2, channel 6, two clients. |
| Password | Eight digits from `esp_random`, new every time it starts, shown on the Update screen. Nothing is stored. |
| Address | `http://192.168.4.1` |
| Off | The toggle; 10 minutes with no page request (`core::wifi::kIdleOffMs`); or moving faster than 3 mph. |

**The page** (`services/src/wifi_page.cpp`, no external files) can:

- **upload** a `dash-*.bin` into the dev slot;
- **download** the ride logs from `logs/` on the card;
- **download** the stable and dev images as a backup;
- **delete** ride logs (`DELETE /logs/<name>`), except the one the logger
  has open, which answers 409;
- **read and change every CONFIG setting** (`GET /api/settings`,
  `POST /api/set?key=&value=`), grouped as CONFIG groups them. Actions are
  not offered. A change is posted as the same `ConfigSet` command the
  config screen posts, so `logic` applies it through the table's clamp and
  `keeper` saves it; the answer is the clamped value.

**The rules, which are the point of the design:**

- **`ota` stays the one writer of the dev slot.** The upload's bytes go
  through a 16 kB stream buffer (`Deps::ota_stream`) to `ota`, which reads
  them exactly as it reads `update/dash.bin` from the card: header check,
  board check, parked-only, a sector at a time, the image's own hash at
  `esp_ota_end`. A stalled upload (10 s) is abandoned and the slot left
  invalid.
- **Nothing on the network can boot anything.** An upload only fills the
  dev slot. Boot dev once and Promote are buttons on the dash.
- **Downloads and deletes cannot leave `logs/`.** `core::wifi::valid_log_name` takes
  only a plain `*.csv` name: no slash, no `..`, one dot.
- **The policy is host-tested** (`core/wifi/`, `test_wifi`); the service is
  the radio and the server.

**Memory.** The SDIO link, lwIP and NimBLE all draw on internal RAM, and
an upload is a burst of SDIO receive buffers. On the demo board a 300 kB
canvas in internal RAM left them 17 kB, and the first upload asserted in
`sdio_rx_get_buffer`. `wifi` logs internal free RAM at every change of
state; the hotspot up costs about 20 kB. Check that line first on the bike
board, whose render task uses only small DMA bands.

**Demo-board caveat.** Its DSI panel scans its frame buffer out of PSRAM,
and a flash write suspends that cache, so the panel flickers for the whole
of an upload. `CONFIG_SPIRAM_XIP_FROM_PSRAM` (IDF's suggested fix) asserted
at startup creating the main task and was reverted (release 1.1, kept as a
record). The bike's i80 panel keeps its own GRAM and does not flicker.

**What the two images share, and the rules for it:**

- **NVS.** Dev must never write anything stable cannot read. CONFIG keys are
  permanent IDs, enforced by `test_config`'s shipped-key list; any new NVS
  data follows the same rule.
- **The C6.** Its firmware is in one place, and an upgrade outlives the dev
  image that pushed it. `c6_firmware.cpp` refuses to move the C6 across an
  ESP-Hosted major version from a dev image.

**The bootloader is nearly full**: 0x5B70 of 0x6000 with rollback, about
1.1 kB free. If it ever overflows, drop `CONFIG_BOOTLOADER_LOG_LEVEL` to
WARN before moving the partition table — and only before the first flash.

`sdkconfig.defaults` carries the whole block, with comments: the
ESP-Hosted SDIO settings copied from `demo_p4/` (do not trust that
component's choice defaults), then NimBLE as host only
(`CONFIG_BT_CONTROLLER_DISABLED=y`, since the controller is the C6's),
pinned to core 1, peripheral and broadcaster roles only, one connection,
MTU 256, bonds persisted to NVS. The partition table already has `slave_fw`
for the C6's own firmware.

### 11.8 Failure rules

- If the radio fails to start, log it, mark `ble_host` as `Fault`, and
  carry on. The dash must never wait on Bluetooth.
- Never block a publish on a BLE callback. The `ble` task copies and
  returns.
- Media text is untrusted input from the phone. Bound every copy to the
  buffer size and terminate it. A 300-character song title must not be
  able to overflow a widget.
- If the phone drops, keep the last title on screen for about 10 seconds,
  then clear it. Do not leave a stale song showing for the whole ride.
- NimBLE's callbacks run on its host task and only queue what happened.
  `ble` drains the queue and is the one writer of `media` and `ble_diag`.
  A full queue drops the event and counts it. It never waits.

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

What you can write and prove **before the PCB arrives**:

| Test | Proves |
|---|---|
| `test_nav` | Every button path through 5 modes and 14 screens |
| `test_lap` | Line crossing, arm, disarm, best lap, delta, GPS loss |
| `test_attitude` | Lean filter against a synthetic turn, drift bounded |
| `test_jump` | Airtime from a recorded free-fall trace |
| `test_trip` | Odometer rollover, tripmeter reset, unit conversion |
| `test_shift` | Threshold curve, hysteresis, no flicker at a boundary |
| `test_geo` | Distance and bearing against known coordinates |
| `test_config` | Range clamping, NVS key stability, round-trip |
| `test_tpms` | Each advertisement decoder against captured hex |

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
  R34/C19 filter may swallow its spike; `p4-board-pin-map.md` §6.3.

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
- Lap delta needs a reference lap trace stored per track. Store the best
  lap of the session, or a saved reference on the SD card?
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
- Which TPMS sensors? The decoder is trivial once you know the layout, but
  it is guesswork until you have a sensor to sniff. Buy the set before you
  write `core/tpms/`.
- ~~OTA over Wi-Fi through the C6?~~ **Built, §11.7.1:** an on-demand
  hotspot and one page, not a station joining a network.
- Should the media strip show on any screen other than Cruise big-speed?
  Track mode should probably stay clean.
