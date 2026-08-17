# Dashboard V2 — System Architecture

Target: ESP32-S3-WROOM-1 N16R8, ESP-IDF v5.x, FreeRTOS, C++17.
Status: design only. No PCB yet.

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
| BLE | In. NimBLE. Phone song metadata and TPMS. No Wi-Fi. |

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
│   ├── bsp/                     # pins.hpp, buses.cpp, board_init.cpp
│   ├── hal/                     # abstract interfaces only, no code
│   │   ├── i_imu.hpp  i_gnss.hpp  i_engine.hpp
│   │   ├── i_gpio_expander.hpp  i_storage.hpp  i_clock.hpp
│   ├── drivers/
│   │   ├── icm42688p/           # SPI, FIFO, data-ready interrupt
│   │   ├── pa1616d/             # UART, NMEA parse, PPS
│   │   ├── mcp23s17/            # SPI expander, on board
│   │   ├── mcp23017/            # I2C expander, handlebar pod
│   │   ├── ili9488/             # esp_lcd panel, RGB666 in SPI mode
│   │   ├── tach_capture/        # MCPWM or GPIO edge capture
│   │   ├── hall_capture/        # front and rear wheel edges
│   │   ├── backlight/           # LEDC PWM
│   │   └── sdmmc/               # card mount, hotplug
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
│   ├── services/
│   │   ├── sensor_hub/          # tach, wheels, shift output, control
│   │   ├── gnss_service/
│   │   ├── imu_service/
│   │   ├── engine_service/      # null now, CAN later
│   │   ├── input_service/       # pod poll, debounce, event queue
│   │   ├── ble/                 # NimBLE host glue
│   │   │   ├── ble_service.cpp  # start, advertise, bond, health
│   │   │   ├── media_gatt.cpp   # the one Feed characteristic (§11.1)
│   │   │   └── tpms_scan.cpp    # passive scan, calls core/tpms decoders
│   │   ├── logger/              # ring buffer to SD
│   │   ├── storage/             # NVS + SD files + USB MSC owner
│   │   └── health/              # per-device state, retry timer
│   └── ui/
│       ├── canvas/              # framebuffer, blit, text, primitives
│       ├── widgets/             # BigNumber, Bar, Gauge, Map, Table...
│       ├── screens/
│       │   ├── cruise/  track/  stat/  diag/  config/
│       ├── theme.hpp            # colors, fonts, one place
│       └── render_task.cpp
└── test/                        # host build: cmake + Catch2 + ctest
    ├── CMakeLists.txt           # plain cmake, does not use idf.py
    └── test_lap.cpp  test_attitude.cpp  test_nav.cpp  ...
```

`core/` and `test/` are the parts you can write today, before the PCB lands.

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
struct Engine   { float rpm; bool neutral; float coolant_c;
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
struct Health   { DevState imu, gnss, mcp_board, mcp_pod, sd, ecu,
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

Core 0 does acquisition and control. Core 1 does the display and the card.
There is no Wi-Fi and no BT, so core 0 is yours.

| Task | Core | Prio | Rate | Owns |
|---|---|---|---|---|
| `ctrl` | 0 | 18 | 500 Hz | tach math, wheel speed, shift-light output |
| `imu` | 0 | 15 | 200 Hz, INT | ICM-42688-P FIFO, attitude filter, jumps |
| `gnss` | 0 | 12 | UART event | NMEA parse, PPS time, position |
| `logic` | 0 | 10 | 50 Hz | lap timer, trip, fuel, command drain |
| `input` | 0 | 9 | 100 Hz | I2C pod poll, debounce, event queue |
| `render` | 1 | 6 | 30 Hz | frame snapshot, widgets, TFT SPI |
| `logger` | 1 | 5 | drain | ring buffer to SD |
| `ble` | 1 | 4 | event | media Feed characteristic, TPMS scan (see §11) |
| `keeper` | 1 | 3 | 1 Hz | NVS save, health retry, USB MSC state |

Nine tasks of your own. Each one has a single job you can say in one line.
Stack sizes and high-water marks go on the DIAG system screen.

The IDF also creates the BLE controller task (priority 23) and the NimBLE
host task. **Both must be pinned to core 1.** The controller outranks
every task you write, so on core 0 it would delay the shift lights. See
§11.4.

The core split therefore is: **core 0 is a real-time island** with no
radio and no filesystem in it. **Core 1 takes everything that can afford
to be late** — display, card, Bluetooth.

### 5.1 Bus ownership

- **SPI2** — IMU, SD card, MCP23S17. Three tasks touch it (`imu`,
  `logger`, `ctrl`). This is safe. The IDF `spi_master` driver serialises
  transactions per bus on its own. Do **not** call
  `spi_device_acquire_bus()` anywhere, or you break that.
- **SPI3** — TFT only. Owned by `render`. DMA, no contention.
- **I2C0** — handlebar pod only. Owned by `input`.
- **UART1** — GPS only. Owned by `gnss`.

### 5.2 Latency budget

Tach edge to shift lamp must stay under about 3 ms, as before. Path:
GPIO edge → `ctrl` at 500 Hz (2 ms worst) → one MCP23S17 SPI write
(~20 µs). `ctrl` runs at priority 18, above everything. The render task
cannot delay it, because it is on the other core and lower priority.

---

## 6. Boot order and the reset trap

`TFT_RST` is on MCP23S17 **GPB6** and `SD_CARD_DETECT` is on **GPB7**, so
the display's reset line lives behind the SPI expander.

R3 is a 10 k pull-up on `TFT_RST`, so the panel releases itself from reset
whether or not the MCP ever initialises — a dead expander does not brick
the screen. But firmware cannot issue a *controlled* reset pulse without
the MCP, and the ILI9488 wants a real hardware reset at power-up rather
than its internal POR. So the order is fixed:

1. `bsp::init_buses()` — SPI2, SPI3, I2C0, UART1, LEDC.
2. MCP23S17 — set direction, drive all shift lamps off, hold `TFT_RST` low.
3. ILI9488 — release `TFT_RST`, panel init, backlight PWM to a low duty.
4. Show a boot screen at once. Do not wait for the sensors.
5. IMU, GPS, SD, pod — each in turn, each reporting to `health`.
6. Start the tasks.

If step 2 fails, log it loudly, mark `mcp_board` as `Fault`, and carry on
to step 3 anyway. The panel will still light. What you lose is the reset
pulse, the shift lamps, the neutral input and card detect — all
degradations the health table already describes.

**Before the first power-on**, confirm the display FPC's IM strap pins
against the panel datasheet. FPC pins 1-4 and 8 are unconnected in the
netlist, and pin 8 sits between two pins tied to VCC. If that is a
floating IM line, the panel comes up in the wrong interface mode and never
answers SPI — which looks identical to a dead panel.

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
| A (top) | Next mode | Jump to Cruise, screen 0 |
| B | Next screen in mode | Previous screen |
| C | Up / previous | Repeat while held |
| D | Down / next | Repeat while held |
| E | Accept / select | Context action for the screen |

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

`Label` · `BigNumber` · `Bar` · `ArcGauge` · `Sparkline` · `Table` ·
`MapView` · `StatusDot` · `DeltaBar` · `LeanIndicator` · `ShiftStrip` ·
`MediaStrip` · `AlertOverlay`

`AlertOverlay` is mode-independent. A TPMS low-pressure warning draws over
whatever screen is up. It is the only thing allowed to break the
one-screen-at-a-time rule, and it needs a dismiss action on button E.

A widget's `set()` compares to the last value and only calls `touch()` on
a change. That gives you the old dirty-flag win, but in one place instead
of scattered through every screen.

### 7.5 Layout

No layout engine. Each screen file declares `static constexpr Rect`
values at the top. You can read the whole layout of a screen in ten lines.

### 7.6 ILI9488 in SPI mode — one gotcha

The ILI9488 does **not** accept RGB565 over 4-wire SPI. It needs 18-bit
(RGB666, 3 bytes per pixel). TFT_eSPI hid this from you. With `esp_lcd`
you need a panel driver that converts on the fly. Use the
`esp_lcd_ili9488` component from the IDF component registry rather than
writing it. Budget: a full 320×480 frame is 460 kB at 3 bytes per pixel,
so draw in bands. Two 40-line bounce buffers in PSRAM is a good start.

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

```cpp
struct LogRecord {          // 32 bytes, fixed
    uint32_t t_ms;
    uint8_t  type;          // sample, event, marker
    uint8_t  flags;
    uint16_t seq;
    uint8_t  payload[24];
};
```

Write a binary format, not CSV. It is 4× smaller, and a small Python
script on your PC turns it into CSV or a chart. Put a header record with a
schema version at the start of every file, so old logs stay readable when
you add a field.

Blackbox is the same ring, kept always full. On an event (a hard landing,
a fault, an ignition-off), flush the last N seconds to a separate file.

**Hardware gap.** There is no VIN sense divider into an ADC on the current
netlist, so the firmware cannot see the ignition turning off before the
rail collapses. Consequences:

- You cannot flush the blackbox on power loss.
- The FAT filesystem can be left dirty by a sudden cut.

Mitigation now: pre-allocate log files, call `fsync` every second, and
never leave a file handle open across a long idle. Fix for v3: one
resistor divider from VIN to a spare ADC pin, plus enough bulk
capacitance on the 3V3 rail for about 50 ms of run-on.

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
block device. Note the TPS2117 gives USB priority over the bike rail, so
VBUS being present does **not** mean the bike is off. Do not use VBUS
alone as the trigger. Make it a CONFIG mode action, with an automatic
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

**The dashboard PCB now at fab has no CAN transceiver.** Its part list
has no SN65HVD230, no TJA1050, no MCP2551. The ESP32-S3 has a TWAI
controller, but TWAI is logic-level TX/RX only. It cannot drive a
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

The dash has three clean spare pins — U2-6, U2-31, U2-32 — which is
enough for a UART pair with one to spare, or for TWAI TX/RX into a
dead-bug transceiver if you want to prove the path before v2.

Design the frame format now, whichever transport wins. The transport is
20 lines; the decode and the interface are the part worth getting right.

Tach RPM stays separate and always available. It comes from the inductive
pickup on `IO17`, not from the ECU. Keep it that way even after the
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

The ESP32-S3 has **BLE 5.0 only**. It has no Bluetooth Classic radio at
all. That rules out AVRCP, which is the normal way a car stereo gets track
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
| `S` | State | `0` stop, `1` play, `2` pause |

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
- **On disconnect, `ble` must publish a zeroed `Media`.** A dash still
  showing what was playing forty minutes ago is worse than one showing
  nothing, and nothing downstream can tell stale from current.

`Media.src` records which path fed the data, so the DIAG screen can say.
There is only one path today; the field stays because a second one would
otherwise be indistinguishable.

**Not doing: Apple's AMS.** An iPhone exposes track metadata natively over
GATT (service `89D3502B-0F36-433A-8EF4-C502AD55F8DC`), needing no app —
but it needs the dash to be a GATT *client* and a BLE *central*, which is
a second connection path to write and debug for a phone nobody here
carries. If that ever changes, this is the note that says where to start.

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

The dash needs two BLE roles at once:

- **Peripheral** — advertises the Dash Media service; the phone connects
  in and writes to it.
- **Observer** — passive scan for TPMS advertisements.

**Not Central, and not a GATT client.** Both were only there for AMS
(§11.1), and dropping them is worth real flash: NimBLE and the controller
together add about 268 kB of image and 33 kB of internal RAM as configured
today, measured by linking them in. `CONFIG_BT_NIMBLE_ROLE_CENTRAL` and
`CONFIG_BT_NIMBLE_GATT_CLIENT` are both still `y` in `sdkconfig` and can
go when the radio work starts.

NimBLE supports this. The cost is radio time. Scanning and an active
connection compete for the same antenna, so duty-cycle the scan:

| Setting | Value | Why |
|---|---|---|
| Scan interval / window | 300 ms / 30 ms | 10% duty. TPMS beacons repeat, so you catch them within a few seconds. |
| Connection interval | 100–200 ms | Metadata is not latency-critical. Long intervals save radio time and phone battery. |
| Max connections | 2 | Phone, plus headroom. |
| Bonding | Stored in NVS | So it reconnects without re-pairing every ride. |

### 11.4 Task and core placement

**This is the part that can break your shift lights.** The BLE controller
task runs at FreeRTOS priority 23 by default, which is **above** your
`ctrl` task at 18. If the controller runs on core 0, it can delay the
tach-to-shift-lamp path past the 3 ms budget.

Pin both the controller and the NimBLE host to **core 1**:

```
CONFIG_BT_CTRL_PINNED_TO_CORE=1
CONFIG_BT_NIMBLE_PINNED_TO_CORE=1
```

Core 0 stays a clean real-time island: `ctrl`, `imu`, `gnss`, `logic`,
`input`. Core 1 absorbs the radio alongside `render`, `logger`, `keeper`.
The render task will be preempted sometimes and drop a frame. At 30 Hz
with a 33 ms budget, that is invisible.

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
               uint8_t state;        // 0 stop 1 play 2 pause
               uint8_t src;          // 0 none 1 phone app
               uint32_t updated_ms; };   // 0 means no media at all

struct TpmsWheel { float psi, temp_c; uint8_t batt_pct;
                   bool alarm; uint32_t age_ms; bool valid; };
struct Tpms { TpmsWheel front, rear; };
```

Both go in `Slots` as their own `Published<T>`, written only by `ble`.
`Media` is 188 bytes, so the seqlock copy is cheap enough at 30 Hz.

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
- **Stat → all data.** TPMS front and rear pressure and temperature.
- **Diag → a new BLE screen.** Connection state, bonded device, RSSI,
  last line accepted on the Feed characteristic, last TPMS advertisement
  age per wheel, decoder hit and miss counts. This screen is what you will actually use to work out
  why a sensor is not showing up.
- **Config.** Forget phone, pairing mode, learn front sensor, learn rear
  sensor, TPMS low-pressure alarm thresholds.

A TPMS low-pressure alarm should be a mode-independent overlay, not a
screen. Losing air is worth interrupting the trip map for.

### 11.7 Partition table and sdkconfig

BLE adds roughly 300 kB of flash and needs NVS space for bonding keys. You
have 16 MB, so this is easy, but the table must be set before you flash
the first time. Suggested `partitions.csv`:

```
# name     type  subtype   offset    size
nvs        data  nvs       0x9000    0x10000    # 64K — settings + BLE bonds
phy_init   data  phy       0x19000   0x1000
coredump   data  coredump  0x1a000   0x10000    # 64K — panic dumps for DIAG
factory    app   factory   0x30000   0x600000   # 6M
```

No OTA partitions. With no Wi-Fi, an update means USB-C, and you already
wanted to flash over USB. BLE OTA is possible later, but it is slow and it
is not worth two 6 MB partitions today. If you think you may want it, add
`ota_0` and `ota_1` now — changing the table later wipes NVS, which means
losing your odometer.

`sdkconfig.defaults` additions:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_PINNED_TO_CORE=1
CONFIG_BT_CTRL_PINNED_TO_CORE=1
CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y
```

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
`components/core/**` with g++ and links Catch2.

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

## 16. Pin map — verified against `FInal_v1.net`

Every line below is read from the shipped netlist, not assumed. Copy it
into `bsp/pins.hpp` and treat that file as the only place these numbers
appear.

| Function | Pin |
|---|---|
| SPI2 MOSI / SCLK / MISO | IO11 / IO12 / IO13 |
| CS: IMU / SD / MCP23S17 | IO14 / IO21 / IO47 |
| SPI3 TFT MOSI / SCLK / MISO | IO10 / IO9 / IO40 |
| TFT CS / DC | IO48 / IO7 |
| TFT RST | MCP23S17 GPB6 |
| Backlight PWM | IO15 |
| GPS UART RX / TX | IO4 / IO5 |
| GPS PPS | IO18 |
| IMU data ready (INT1) | IO8 |
| Tach input | IO17 |
| Wheel hall front / rear | IO1 / IO2 |
| Shift-lamp master PWM | IO16 |
| Shift lamps 1-8 | MCP23S17 GPA0-GPA7 |
| Neutral input (**active LOW**) | MCP23S17 GPB5 |
| SD card detect | MCP23S17 GPB7 |
| I2C SDA / SCL (pod) | IO41 / IO42 |
| USB D− / D+ | IO19 / IO20 |
| Boot button / reset | IO0 / EN |
| Debug UART RX / TX | IO44 / IO43 |
| Spare ESP32 pins | **IO6 (ADC1)**, IO38, IO39 |
| Strapping pins, usable with care | IO3, IO45, IO46 |
| Do not use | IO35, IO36, IO37 — octal PSRAM |
| Spare MCP pins | GPB0-GPB4, INTA, INTB |

Notes that change firmware:

- **IO6 is the only spare pin on an ADC.** IO38 and IO39 have no ADC at
  all, and IO1/IO2 are ADC1 channels already spent on the wheel Halls. So
  every future analogue input — a VIN sense divider above all — has to go
  to IO6. Do not spend it on something digital.
- **Neutral reads LOW.** The PC817B pulls GPB5 down through R64 when the
  bike's neutral switch closes.
- **The pod has no interrupt line.** CN1 is SDA, SCL, VCC, GND only, so
  buttons must be polled. The 100 Hz `input` task is required, not a
  choice.
- **Run the pod I2C at 100 kHz.** With 4.7 k pull-ups over 0.5 m of cable
  the rise time is roughly 470 ns. Fast mode allows 300 ns; standard mode
  allows 1000 ns. 400 kHz is out of spec on this harness.
- **The MCP23S17 caps the shared SPI bus at 10 MHz for its own
  transactions.** Give each device its own `spi_device_interface_config_t`
  clock — the IMU can take 24 MHz and the SD card more, and the IDF driver
  reconfigures per transaction.
- GPB0-GPB4 are free. If the handlebar I2C link proves unreliable, you can
  move the five buttons onto those pins and run the pod on the SPI
  expander instead. That is a wiring change, not a firmware rewrite, as
  long as `input_service` sits behind `i_gpio_expander.hpp`.

### 16.1 Two hardware limits the firmware must live with

**No power-source sense.** There is no VIN divider to an ADC, and the
TPS2117 `ST` pin goes only to LED10, not to a GPIO. So firmware cannot
read battery voltage, cannot tell bike power from bench USB, and cannot
see the ignition go off. Consequences: the `Elec` slot has no source, the
blackbox cannot flush on power loss (§9.2), and the USB-MSC handover
(§9.3) must trigger on "USB enumerated **and** RPM is zero" rather than a
real test. One divider to IO6 fixes all three on the next revision.

**USB bench current.** D7 feeds the 5 V rail from VUSB, so on the bench
everything runs from the port: roughly 610 mA with all eight shift lamps
on, against a 500 mA USB 2.0 limit. Clamp the IO16 shift-lamp PWM duty
when there is no engine RPM. This also keeps the AMS1117 out of thermal
trouble.

---

## 17. Build order

1. `core/` plus `test/`. Nav, lap, geo, units, trip, shift. No hardware.
2. `bsp/` plus `drivers/mcp23s17` plus `drivers/ili9488`. Boot to a
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

- Backlight: auto-dim from a light sensor, or a manual CONFIG setting?
  There is no light sensor on the board today.
- Lap delta needs a reference lap trace stored per track. Store the best
  lap of the session, or a saved reference on the SD card?
- Track map: auto-detect the track outline from the first lap, as in V1,
  or load a track file from SD?
- Does the trip map in Cruise mode need any base map, or just a
  breadcrumb trail? A base map means tiles on the SD card and a lot of
  work.
- ~~Which phone do you carry?~~ **Settled: a Sonim XP3900, an Android
  handset.** So there is no AMS path and a phone-side app is required.
  The dash side is one write characteristic (§11.1); the app is a
  separate piece of work and should wait until the radio works on the
  dash, or you are debugging two unproven ends at once.
- Which TPMS sensors? The decoder is trivial once you know the layout, but
  it is guesswork until you have a sensor to sniff. Buy the set before you
  write `core/tpms/`.
- Do you want OTA over BLE later? If there is any chance, add `ota_0` and
  `ota_1` to the partition table now. Adding them later wipes NVS.
- Should the media strip show on any screen other than Cruise big-speed?
  Track mode should probably stay clean.
