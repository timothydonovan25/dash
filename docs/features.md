# CRF250F Dashboard — Feature Reference

Every feature of the v2 firmware, its tools and its simulator, as of
firmware **1.62** (`bike-doom`), 2026-11-02. This is the reference for
*what exists and how it behaves*; `CLAUDE.md` keeps only the rules and the
traps, and `system-architecture.md` the argument for the shape.

**Read this first.** Everything below is *written*. The parts under
`components/core/` and `components/ui/` are host-tested (`test/test_*.cpp`)
and run in the simulator. The parts that talk to hardware compile and link
for the ESP32-P4, and **only some of it has run on the bike board**:
`docs/bringup.md`'s status table is the one record of what has. Where a
feature is a placeholder or runs on guessed thresholds, its section says so.

Each section gives what the feature does, where it lives in the code, and
how a rider reaches it. Paths are relative to the repo root.

### Find a feature by its code

| Code | § | Code | § |
|---|---|---|---|
| `core/alert` | 11.1 | `core/log` (blackbox, detail) | 23 |
| `core/attitude` | 14.1 | `core/media`, `core/media/apple` | 24 |
| `core/battery`, `core/power` | 21 | `core/nav` | 3, 9 |
| `core/ble` (pairing) | 24 | `core/nmea`, `core/fixtime` | 16, 7.1 |
| `core/c6` (C6 firmware push) | 24 | `core/ota` | 26 |
| `core/crash` (crash SOS) | 24.3 | `core/ecu` (K-line adapter link) | 35 |
| `core/config` | 10, 34 | `core/perf` | 19 |
| `core/delta` | 7.3 | `core/session` | 7.6 |
| `core/flash` (`still`) | 28 | `core/shift`, `core/tach` | 12 |
| `core/fusion` | 13 | `core/slip` | 11.3 |
| `core/gear`, `core/gearcal` | 15 | `core/state` (slots, Frame, Command) | 2, 34 |
| `core/geo`, `core/lap` | 7 | `core/stunt` | 20 |
| `core/health` | 22 | `core/sun` | 27 |
| `core/hr` | 24.2 | `core/sweep` | 5 |
| `core/input` | 3 | `core/track` | 7.5 |
| `core/jump` | 14.2 | `core/trip`, `core/units` | 17 |
| `core/waypoint` | 18 | `core/wifi` | 25 |
| `core/route`, `services/route_store` | 6.5, 18 | `tools/routegraph` | 18 |
| `ui/` widgets, `Shadow`, fonts | 4 | `ui::Toast` | 11.2 |
| `components/doom` | 31.1 | `sim/` | 29 |
| `tools/`, `mount/` | 30 | `demo_p4/` | 31 |

---

## Contents

1. [Hardware platform](#1-hardware-platform)
2. [Software architecture](#2-software-architecture)
3. [Controls: the five-button pod](#3-controls-the-five-button-pod)
4. [Display and rendering](#4-display-and-rendering)
5. [Startup](#5-startup)
6. [CRUISE mode](#6-cruise-mode)
7. [TRACK mode: lap timing](#7-track-mode-lap-timing)
8. [STAT mode](#8-stat-mode)
9. [DIAG mode](#9-diag-mode)
10. [CONFIG mode](#10-config-mode)
11. [Overlays: alerts, toasts, slip bar](#11-overlays-alerts-toasts-slip-bar)
12. [Shift lights and tachometer](#12-shift-lights-and-tachometer)
13. [Speed: wheel sensors and sensor fusion](#13-speed-wheel-sensors-and-sensor-fusion)
14. [Attitude: lean, pitch, jumps](#14-attitude-lean-pitch-jumps)
15. [Gear indication and calibration](#15-gear-indication-and-calibration)
16. [GPS](#16-gps)
17. [Trip computer, fuel and engine hours](#17-trip-computer-fuel-and-engine-hours)
18. [Navigation waypoints](#18-navigation-waypoints)
19. [Performance timers](#19-performance-timers)
20. [Stunt scoring](#20-stunt-scoring)
21. [Battery and power](#21-battery-and-power)
22. [Health monitoring and fault recovery](#22-health-monitoring-and-fault-recovery)
23. [Logging](#23-logging)
24. [Bluetooth: media control and phone notifications](#24-bluetooth-media-control-and-phone-notifications)
25. [Wi-Fi hotspot and web page](#25-wi-fi-hotspot-and-web-page)
26. [Firmware updates: stable and dev slots](#26-firmware-updates-stable-and-dev-slots)
27. [Night dimming](#27-night-dimming)
28. [Real-time guarantees](#28-real-time-guarantees)
29. [The desktop simulator](#29-the-desktop-simulator)
30. [Host-side tools](#30-host-side-tools)
31. [The demo board build](#31-the-demo-board-build)
32. [Release and version management](#32-release-and-version-management)
33. [Deliberately not here](#33-deliberately-not-here)
34. [Configuration reference](#34-configuration-reference)

---

## 1. Hardware platform

The firmware targets one custom PCB, built around the Guition
**JC-ESP32P4-M3-C6** module.

| Function | Part | Bus / pin | Notes |
|---|---|---|---|
| Main CPU | ESP32-P4 (dual core) | — | No radio of its own |
| Radio co-processor | ESP32-C6 (MINI-1U) | internal SDIO, ESP-Hosted | BLE and Wi-Fi; needs an external antenna on its IPEX |
| Display | 3.5" ILI9488, 320×480, used landscape 480×320 | 8-bit 8080 parallel (i80) | RGB565, IM[2:0] = 011; D0..D7 on GPIO33..26 (reversed); RST 53, WR 52, DC 51, CS 50, TE 49 |
| Shift lamps + backlight | PCA9955B (U2) | I2C1 at 1 MHz | OUT0–7 lamps, OUT8–10 backlight cathodes |
| IMU | ICM-42688-P | SPI2 | FIFO-driven, 200 Hz |
| GPS | PA1616D | UART1 | NMEA; baud negotiated to 57600 |
| SD card | microSD in SPI mode | SPI2 (own clock) | Shares the bus with the IMU |
| Handlebar button pod | I/O expander over cable | I2C0 at 100 kHz | No interrupt line; polled at 200 Hz |
| Tachometer | Inductive wrap on the plug lead | capture input | 5–10 V signal; R34/C19 reworked to 10 kΩ / 47 pF |
| Wheel speed | Hall sensors, front and rear | capture inputs | Same counter block as the tach |
| Neutral switch | Opto-isolated (U6) | GPIO12 | Reads LOW in neutral |
| VIN sense | R17/R18 divider | GPIO20 | Tells bike power from USB and sees key-off |
| USB-C | P4 USB-Serial/JTAG | GPIO24/25 | Flashing, console, JTAG |

Pin numbers live in one place, `components/bsp/include/bsp/pins.hpp`, and
`docs/p4-board-pin-map.md` documents them. `components/bsp/src/buses.cpp`
brings up SPI2, I2C0, I2C1, UART1 and the i80 bus. A driver only ever adds a
device to a bus that already exists.

Drivers, one per chip, in `components/drivers/`:
`ili9488`, `pca9955b`, `icm42688p`, `pa1616d`, `pod`, `sdcard` (SPI mount,
plus an SDMMC mount on the demo), `engine_sense` (tach and both wheels),
`vin_sense`, `chip_temp`, `nvs_storage`, `clock`. Each reports its health
and leaves policy to someone else: no driver aborts, blocks forever or
retries in a tight loop.

---

## 2. Software architecture

The firmware is a clean rewrite (v2). `docs/system-architecture.md` is its
contract. In summary:

- **Layers.** `bsp` (pins, buses) → `drivers` (chips) → `dash_hal`
  (abstract interfaces: `IEngine`, `IImu`, `IGnss`, …) → `services` (the
  tasks) → `core` (pure logic) and `ui` (screens and widgets).
- **No globals.** A module gets its data from its arguments.
- **One writer per piece of state.** Each producer owns a `Published<T>`
  slot. Readers get a consistent copy without locks. The slot is
  double-buffered, not a seqlock, so a reader never waits on a writer that
  stopped half way.
- **The `Frame`.** `snapshot()` copies every slot into one `Frame`. A screen
  is a pure function of a Frame: it does not read sensors or touch the bus.
- **Commands down.** Changes go downward as `Command`s (set the lap line,
  reset a trip, `ConfigSet`, OTA actions, …) into a queue. The owning task
  applies them.
- **`core/` is host-portable.** It includes no ESP-IDF or FreeRTOS header,
  and time comes in as a parameter. So all of it builds and is tested on a
  laptop.
- **Bulk data** (map traces, lap history, the log ring) lives in ring
  buffers (`TrackBuffer`) outside the Frame.

**Thirteen tasks.**

| Core 0 (real-time) | Core 1 (can be late) |
|---|---|
| `ctrl` 500 Hz + every tach edge — shift lamps, LED driver | `render` 120 Hz — the panel |
| `imu` — IMU FIFO, attitude | `logger` — SD writes |
| `gnss` — NMEA in | `storage` — card, track files |
| `logic` 50 Hz + a pass on each fix or command (`wake_logic`, `LogicPass`) | `keeper` 20 Hz — health, NVS, backlight level |
| `input` 200 Hz — the pod | `power` 1 kHz — VIN, battery |
| | `ota`, `ble` (+ NimBLE host), `wifi` |

---

## 3. Controls: the five-button pod

Five buttons, A to E, on a handlebar pod read over I2C. The pod has no
interrupt line, so `input` polls it at 200 Hz. `core/input/` debounces it:
the reading must be stable for 4 ms, which is two reads. A spike seen by one
read is never a press. A press registers 5–10 ms after it lands.

| Button | Short press | Long press (600 ms, `core::input::Config::long_press_ms`) |
|---|---|---|
| **A** | Next mode, acted on the press | Jump to CRUISE screen 0. Fires on top of the short press, so the escape works from anywhere |
| **B** | Next screen in this mode, acted on the press | — (gave up "previous screen" so it would not lag) |
| **C** | Up / previous, acted on the press | Repeats while held |
| **D** | Down / next, acted on the press | Repeats while held |
| **E** | Accept / select, acted on release | The screen's context action |

E alone waits for its release, because its short and long presses do
different things. Which buttons act on the press is `core::input`'s
`on_press_mask`: a button given a long meaning must leave that mask. Long presses fire when the hold reaches 600 ms, not on
release, so a rider holding E to set the finish line knows when it took.

**Modes**, cycled by A: CRUISE → TRACK → STAT → DIAG → CONFIG.

Screens get first refusal on a button. CONFIG takes C and D while a value is
being edited, and SPEED takes C, D and short E for media control. Anything a
screen does not take goes to the navigation state machine in `core/nav/`.

---

## 4. Display and rendering

- **Panel.** A 480×320 landscape panel on an 8-bit 8080 bus at a 25 MHz
  write clock (clean at 25 and 30 MHz on the first board). A whole frame is
  307 kB in RGB565, about 12 ms on the wire, roughly one and a half render
  periods (31 ms at the 10 MHz it started at). The driver is the
  `esp_lcd_ili9488` component on an i80 IO handle. Two panels are
  supported: the HT035SHV007NS (IPS: it takes the component's `_ips` init;
  the default TN init gave a negative with a crawling speckle) and VIEWE's
  transflective UEED035HV (ST7365, VIEWE's own init table in
  `drivers/ili9488.cpp`, with WR and RS the other way round on FPC pins
  10/11). The first `init()` tells the two wirings apart by which one makes
  TE pulse, trying the one saved last boot first (`hal::kKeyPanelModel`).
- **Partial updates.** `render` sends only the widgets that changed, and
  where a widget can say (`Widget::damage()`), only the part that changed:
  a table's changed rows, the tach marker's rows. Anything that moves is a
  widget. Background chrome is redrawn only on a full repaint. `test_ui`
  checks that a partial update lands the same pixels as a full repaint.
- **Shadow framebuffer** (`ui::Shadow`). A copy of the glass is kept in
  PSRAM, and each strip goes out only where it differs from that copy. This
  trades CPU for bus time: the SPEED screen's riding frame is about 1 ms on
  the wire instead of 2. A screen change over a known glass takes 6–26 ms
  instead of 31. Anything that leaves the glass unknown (a failed blit, a
  panel re-init) invalidates the shadow.
- **Scrub.** `render` resends one band of the shadow every second (3.9 ms
  of bus). This repairs an ILI9488 that has lost its memory, with no write
  failing.
- **Anti-aliased large text** (1.31). At scales 3–6 (all of ASCII) and 8,
  10, 20, 22 (digits, `+-.:%`, N C F R), glyphs from Noto Sans Condensed
  Bold are blended at 4 bits into the 5×7 bitmap's cell. Widths, layout and
  per-character damage stay the same. `tools/gen_font.py` generates
  `font_aa.cpp`. It costs about 350 kB of flash and no RAM.
- **23 widgets** in `ui/widgets.hpp`: Label, ValueBox, BigNumber, Bar,
  StatusDot, ShiftStrip, DeltaBar, LeanIndicator, Sparkline, Table, MapView,
  TachBar, RevBar, LeanGauge, MediaRow, MediaProgress, AlbumArt,
  AlertOverlay, NavArrow, SlipBar, HrGraph, DevTag (the red DEV tag a dev
  image wears) and Toast.
  `ui/chrome.hpp` holds the shared titles, rules and framed tiles, and
  `theme.hpp` is the semantic colour layer.
- **One live screen.** `ui::App` builds the current screen into one slot
  sized for the largest (`AllScreens`) and destroys it on leaving, so the
  App costs about 1.9 kB on render's stack instead of 13 kB. Anything that
  must outlive a screen (map traces, lap history, CONFIG's place) is held by
  the App and handed back.
- **FPS counter** (2.0, CONFIG → Diag → FPS counter, off by default). The
  render rate as `tried/sent` (2.2), e.g. `120/15`: frames a second
  (`SystemInfo::render_hz_x10`, the figure DIAG → System shows) and how many
  of them put anything on the wire (`render_sent_hz`; at least 1, the tag's
  own redraw), small, in the top-left corner over every
  screen (`ui::FpsTag`, owned by the App like the DEV tag). It changes once a
  second, so it costs one tiny blit a second; switched off, the corner is
  painted back as damage. It covers the start of a title bar.
- **Frame rate** (2.1, CONFIG → Diag → Frame rate): `render`'s deadline,
  10, 20, 30, 60 or 120 Hz (the default), or Unlimited. Read every pass, so
  a change takes effect at once. Unlimited still waits one 1 ms tick a
  frame: `render` outranks `ble`, `keeper`, `storage`, `wifi`, `logger` and
  `ota` on core 1, and a loop that never blocked would starve them and the
  idle task the watchdog checks. A button press always cuts the wait short.
  The bike's `render` only; the demo board's loop is its own.
- **Backlight.** Three PCA9955B channels. `keeper` decides the level and
  `ctrl`, the chip's only writer, applies it.

---

## 5. Startup

- **Splash.** 1 s at power-on (`ui::App::kSplashMs`; CONFIG → Startup →
  Splash turns it off).
- **Gauge sweep** (`core/sweep/`). Rpm, speed and gear run to full scale and
  back. It takes 1 s by default (500–5000 ms, CONFIG → Startup → Sweep
  time), with peak rpm 8800 and five gears. The sweep is applied to the
  Frame by `ui::App` and never written to a slot. It aborts if the bike
  moves faster than 1 mph.
- **Restore.** `logic_restore` brings back settings, waypoints, odometer,
  trips and engine hours from NVS. The saved track nearest the first good
  fix is recalled.
- **Boot image.** Power-on always boots the stable slot, with no chooser, so
  nothing slows the boot (§26).
- **Radio last.** `start_ble` comes last in the boot order, so a radio that
  will not come up delays nothing else.

---

## 6. CRUISE mode

Seven screens, cycled by B.

### 6.1 SPEED (screen 0)
The main riding screen. It has no title bar, to leave every pixel for data.
- Big speed. The source is set by CONFIG → Display → Speedo: Wheels or
  Fused.
- Big rpm, a tach bar (stem plus flare section) and an on-screen shift strip
  that mirrors the lamps.
- Gear, from the neutral switch or the gear estimator.
- Clock (GPS time plus the UTC offset).
- Lean gauge.
- GPS satellite count and HDOP.
- **Worst device**: the name of the device in the worst health, with what is
  wrong underneath. Blank when everything is healthy.
- **Now playing**: song, album, artist and a progress bar from the phone.
- Odometer and trip.
- **Media control**: C = previous track, D = next, short E = play/pause,
  sent to the phone over BLE's Ctrl characteristic.

### 6.2 MEDIA
A dedicated now-playing screen. Title, artist and album are large enough to
read, and there is a big progress ring and album art. Speed stays on screen
in a small size so the screen is still useful while moving.

### 6.3 TRIP
Odometer, trip A, trip B, fuel estimate, engine hours and top speed, each in
a framed box. **Long** E resets trip A. A short press does nothing, so a bump cannot reset it by accident.

### 6.4 MAP (trip map)
A live breadcrumb trace of the ride (`MapView` over the trace ring), with
distance, heading and speed.

### 6.5 NAV — turn-by-turn routing (2.x, not yet run on the board)
Offline routing on an OpenStreetMap map from the card (`route/map.rgf`,
built by `tools/routegraph`). A **speed limit** (50 to 75 mph by 5, or
off) keeps routes off roads posted over it; off allows interstates. See
`docs/navigation.md`.

**Picker:**
- The first row is **Where to?**: spell an address, town or place with the
  pod, as on an old Garmin. Only letters that lead somewhere are offered;
  results come nearest first; a street then takes a house number, or GO
  for the street. It works offline from `route/search.idx`
  (docs/navigation.md §7.1).
- Then a destination sent from the phone (Feed `G`, routed to at
  once), the waypoints that are set, the favourites from
  `route/destinations.csv`, the nearest fuel and food, a **Speed limit**
  row (E up 5 mph, hold E down: 50 ... 75, off; CONFIG → Nav → Speed
  limit) and a
  "Dirt roads" switch (CONFIG → Nav → Avoid dirt).
- CRUISE → MAP draws the route, dim, under the ridden trace; stretches
  over the speed limit amber (2.7).
- E goes. Hold E on a waypoint row drops that waypoint here.

**Guidance:**
- A turn arrow, the distance to the turn and the road's name, the posted
  limit (held 2 s on a change, so it does not flicker at junctions), and
  the distance and minutes left. CRUISE → SPEED shows the limit too, under
  the heart rate.
- Off the route for 5 s, it reroutes. On a road over the speed limit, it
  routes along it, and back under the limit once off it (2.6).
- Short E switches to the **route view** and back (2.7): the whole route
  fitted to the screen, north up, blue with stretches over the limit amber, the
  destination ringed and the bike a white dot and arrow. The distance and
  time left stay below it.
- The route view names the route's **main roads** (2.18; 2.17 named the
  next three turns): the two the route spends longest on, plus a third and
  fourth if each takes a tenth of its time or more. A dot sits in the middle
  of each road's longest stretch with the name beside it, 15 characters at
  most. A name tries right, left, above and below its dot, then the same at
  half size (2.19); with no room clear of a bigger road's name, the dot
  shows alone. Picked once per
  route (`core::route::longest_roads`, in `route_store`), placed by
  `logic`'s `label_route`, carried in `Nav::labels`.
- **Pace notes** (2.8): the next bends on the road graded rally style,
  1 (tightest) to 6, with into / tightens / opens / long / hairpin, on the
  status line: `300 R4 tightens 150 L3 into R5` (docs/navigation.md §7.2).
  From the map's shape only; first-guess thresholds, not yet ridden.
- Hold E stops.
- When nothing connects under the speed limit: it says so with the route
  view up; E routes at any speed for this destination, hold E stops.

### 6.6 MINIMAL
Speed and the time, with no title, tach, lean or tiles. For long road
sections.

### 6.7 PHONE
The phone's last call, missed call or message (§24). The message text shows
only while stopped, unless CONFIG → Phone → Text moving allows it.

---

## 7. TRACK mode: lap timing

Five screens: **RACE, LAP, LAPS, MAP, SESSION.**

### 7.1 The lap timer (`core/lap/`, `core/geo/`)
- **The finish line** is a segment through the bike's position when it is
  set, perpendicular to its heading. Its half-width defaults to a setting
  (CONFIG → Lap → Line width, 10–200 ft). Hold E on RACE or LAP, or use
  CONFIG → Lap → Set line here.
- **Crossing detection** uses sub-sample interpolation between fixes, so the
  crossing time falls between two GPS samples.
- **Gates** turn a geometric crossing into a real lap:
  - Arm speed: the bike must be above a set speed (0–60 mph).
  - Min lap: a lap shorter than the minimum (2–600 s) is rejected.
  - Fix gap: a crossing across a long loss of GPS is not scored. At 1 Hz,
    one lost fix is not a gap and jitter is not silence (1.45).
- **Fix timing** (`core/fixtime/`, 1.24). Each fix is timed by its own GPS
  time, mapped onto the system clock with an NTP-style lowest-latency
  offset, not by when it arrived. This took lap error from up to 17 ms to
  about 2 ms.
- **Arming.** Tap E to re-arm.
- **Clear laps** (CONFIG → Lap) drops the session's laps and stops track
  recall until the next power-on.

### 7.2 Sectors (`core/lap/sectors.hpp`, 1.26)
- Two split lines are placed automatically at one third and two thirds of
  the first clean lap's distance. There is no rider setup, and it works on
  any track.
- They are timed in order with the finish line's crossing.
- The firmware tracks each sector's best and the **ideal lap**, the sum of
  the best sectors.
- They are forgotten when the line is set or cleared, and restored from a
  saved track.

### 7.3 Lap delta (`core/delta/`) — placeholder
A live gain or loss against the quickest clean lap known, from this session
or from the saved track. Matching is nearest-point on the reference trace,
searched in a window around the last match, and quantised to a tenth of a
second. **Every threshold is a guess that has never seen a real GPS trace.**
Tune it on the first track day.

### 7.4 Peaks (`Peaks`)
Each lap's top speed, top rpm and peak lean go into `LapHistory`. Lean
counts only above 10 mph. The session's lean each way is kept too, and the
IMU screen shows it.

### 7.5 Track memory (`core/track/`, 1.30)
- **A track is its finish line.** Setting a line saves it to the card as
  `tracks/tNN.trk`, with 16 slots. The file holds the splits the first clean
  lap places, the record, each sector's best and the reference lap.
- **Auto-recall.** A good fix within 1500 ft of a saved line brings it all
  back with no button pressed, and the delta works from the first lap.
- **Moving a line.** A line set within 300 ft of a saved one is that track's
  line moved, and its record starts over.
- **Forget track** (CONFIG → Lap) deletes the saved file.
- `logic` cannot touch the card, so `storage` does the file work through
  `services::TrackMailbox`, which holds one job at a time and keeps the
  library in PSRAM.

### 7.6 The screens
- **RACE** is the track-day screen. It has no title bar. A full-width rev
  bar (`RevBar`) is lit the colour the shift lamps are, the gear is drawn at
  scale 20, and the screen shows the running lap, the delta, and the last
  and best laps. E works as on LAP.
- **LAP** shows the current lap, last, best and ideal, a delta bar, the lap
  count, speed, the timer's phase (unarmed, armed, timing) and the SPLITS
  tile. **LAST flashes for 5 s on a new best.**
- **LAPS** is a table of every lap with its time and its peaks (top speed,
  rpm, lean), with the best marked.
- **MAP** (track map) shows the track trace, the lap, the distance to the
  line and a delta bar.
- **SESSION** (`core/session/`) is the paddock summary: best lap, ideal lap,
  track record, distance, riding time, top and average speed, jump count and
  the longest jump, and altitude.

---

## 8. STAT mode

Six screens: **ALL, ENGINE, IMU, PERF, STUNT, HEART.**

- **ALL** — every live value in one table.
- **ENGINE** — rpm, engine hours, throttle position with a bar, voltage,
  and an rpm sparkline sampled every 100 ms. Throttle is the ECU's, and
  reads `--` without the K-line adapter; the line across the top says
  whether the adapter and the ECU are heard, and carries the ECU's engine
  and intake temperatures and manifold pressure (§35).
- **IMU (attitude)** — a lean indicator, pitch, yaw rate, lateral g and
  vertical g, airtime, best airtime and landing g, plus the session's peak
  lean left and right. Short E resets the lean peaks (`LeanPeaksReset`).
- **PERF** — the performance timers (§19). E clears the bests.
- **STUNT** — wheelie and stoppie scoring (§20). E starts a new session
  score.
- **HEART** — the watch's heart rate, zone, session average/high/low, a
  ten-minute graph (`ui::HrGraph`) and the RECOVERY tile (§24.2).

---

## 9. DIAG mode

**DIAG's screen 0 is a menu** (1.29). It lists the eight pages, each with a
one-word reading of how it stands. C and D move the cursor, E opens a page,
and B on a page returns to the menu with the cursor where it was. The menu
flags a core dump left in flash. A ninth row starts Doom (§31): on the
bike only while it is still, reading "parked only" otherwise.

| Page | Shows |
|---|---|
| **SENSORS** | A status dot and state for each device: IMU, GNSS, LED driver, pod, SD, ECU, display, BLE host, phone, TPMS front and rear, front wheel, rear wheel |
| **GPS** | Fix, fix quality, satellites, HDOP, latitude, longitude, altitude (m), speed, UTC |
| **FUSION** | Fused road speed and its sigma. For each source (IMU, front wheel, rear wheel, GPS): its reading, its residual, and whether it is used, gated or suspect. Accelerometer bias, the wheel verdicts, filter restarts |
| **SYSTEM** | Heap free and low-water mark, PSRAM free, the tightest task's stack, uptime, P4 die temperature (1 Hz), IMU temperature, render rate, log rows and drops, the last crash dump (the task, labelled CRASHED if this boot was the crash), SD card state including "safe to pull" after an eject |
| **POWER** | Supply (bike or USB), VIN, battery volts (with trim), the rest voltage at key-on, cranking low, charging voltage and a verdict (§21) |
| **BUS** | Each bus's state: I2C0 pod, I2C1 LEDs (or the LED driver's own lamp/backlight faults, §12.2), SPI2 IMU, SPI2 SD, UART1 GPS, i80 TFT. Counters: pod NACKs, I2C recoveries (and "still stuck"), IMU missed interrupts and errors, GPS bad checksums, `ctrl` overruns. The neutral input and VIN sense |
| **RADIO** | The BLE host, bonds, pairing window, the connected phone and its RSSI, whether it has subscribed, media feed state, the Wi-Fi hotspot |
| **UPDATE** | Version and build date of both firmware slots, which one is running, and three actions: install from SD, boot dev once, promote to stable. Shows the Wi-Fi password while the hotspot is on (§25, §26) |

---

## 10. CONFIG mode

One screen built from the single `ConfigItem` table in `core/config/`. Its
groups are submenus. Items are numbers (with min, max, step and unit),
choices, booleans or actions. C and D edit and E accepts.

**Settings have one writer at run time, `logic`.** The CONFIG screen, the
Wi-Fi page and IMU's Calibrate level all post `ConfigSet` commands. A task
that keeps its own copy compares and re-reads. `keeper` saves settings to
NVS, and only while the bike is still (§28). §34 lists every item.

**Editing (2.12).** One E flips an On/Off item; numbers and choices still
take E, C/D, E. A long E puts any item back to its default
(`core::config::default_value`), and a `*` after a label marks one that
differs. Actions that lose something (`ConfigItem::confirm`: Clear laps,
Forget track, Reset to base, Reset trip A, Forget phone, Test crash) show
"E again" on the first E and run on the second; any other button cancels.
CONFIG → Shift → **Test lamps** runs the start-up sweep on the lamps once
(`Deps::lamp_test_req`, `logic` to `ctrl`); it stops when the wheels turn.

**Start where you left off (2.12).** The mode and screen showing are saved
(`hal::kKeyUiPlace`, by `keeper` while still) and the dash starts on them.

While a gear calibration run is open, `ui/screen_gearcal.cpp` takes over
CONFIG (§15).

---

## 11. Overlays: alerts, toasts, slip bar

### 11.1 Alerts (`core/alert/`)
`ui::App` evaluates alerts on every Frame and shows one at a time, by
priority:

| Alert | Trigger |
|---|---|
| **No pulse** | The watch, worn this ride, has sent readings with no pulse for 10 s (§24.2) |
| **Bike down** | Leaned past 70° and below 3 mph |
| **Battery high** | Regulator fault, battery cooking near 16 V |
| **Battery low** | Engine running and not charging |
| **Front wheel / Rear wheel** | The speed sensor has gone quiet |
| **GPS lost** | Mid-session, a lap timer with no position |
| **SD card** | The log has stopped |
| **Shift lamps** | The lamp driver is gone, and the backlight with it |
| **IMU** | Lean, jump and lap-delta inputs are gone |
| **Service due** | The engine-hours interval has run out (§17, 2.14). Last, and dismissed with a press until Mark done |

Each alert has an onset delay and a clear delay. A dismissal holds until
the fault genuinely clears. Low fuel is deliberately not an alert: the burn
model is a guess and there is no fuel-reset action to re-arm it.

### 11.2 Toasts (`ui::Toast`, 1.32)
A strip along the bottom of every screen, under the alert. In priority
order:
1. A ringing call
2. A phone notification (8 s)
3. A trick under way (live)
4. A finished trick (3.5 s) or a new perf best (4 s)

Toasts do not appear on STUNT or PERF for those screens' own events.
CONFIG → Display → Popups and CONFIG → Phone → Notifications turn them off.

### 11.3 Slip bar (`core/slip/`)
A bar across the top of every screen for **wheelspin and lockup**. Each
wheel is compared with road speed (fused, with GPS behind it), and the bar
holds at its peak for 0.7 s. CONFIG → Display → Slip bar turns it off.

---

## 12. Shift lights and tachometer

### 12.1 Tach (`core/tach/`)
- Spark pulses become rpm. CONFIG → Shift → Revs/spark is 1 or 2 (wasted
  spark or not).
- **Smoothing in time, not per pulse** (1.46). CONFIG → Shift → Tach smooth,
  0–50 ms.
- An ISR refractory window rejects ringing after each spark edge.
- `tach::RateEstimator` gives the rpm slope, which the lamp lead uses.
- `PulseWindow` counts extra and missed pulses for the detail log.

### 12.2 Shift lamps (`core/shift/`)
- Eight lamps on PCA9955B OUT0–7. `shift::pattern()` maps rpm to a lamp
  pattern.
- Settings: **First lamp** rpm, **Span**, **Hysteresis**, **Flash above**
  rpm, lamp **Brightness**, and **Peak hold** (how long the top pattern is
  held).
- **Lead time** (0–300 ms, off by default). The lamps show where the rpm
  will be, from the slope, less a noise floor.
- **Latency.** The tach ISR wakes `ctrl` on every edge, so the lamps are
  decided at the edge and not at the next 2 ms tick. The budget from tach
  edge to lamp is **under 3 ms** (§28).
- **USB cap.** On USB power (VIN sense low), `ctrl` caps lamp duty.
- **Re-init.** The PCA9955B's reset is tied to the P4's EN, so a software
  reset does not reset it. `init()` rewrites every register, and `ctrl`
  refreshes the config once a second.
- **Lamp and backlight faults (2.10).** Just before that refresh, `ctrl`
  reads the chip's own flags (MODE2, EFLAG0–3) and the refresh clears them,
  so each read covers one second. `core::shift::decode_led_faults` sorts
  them into lamps open/short, backlight strings open/short and
  overtemperature, published as `Engine::led_faults`. DIAG → BUS shows
  them on the I2C1 row (`open 1+4`, `short 2`, `BL 3`, `OVERTEMP`) and
  `_evt` logs a `led_fault` line on each change. Health only: nothing is
  switched off over one. The chip tests an output only while it is lit at
  PWM 8..254, so 100 % is 254, and a lamp is checked once the rpm lights
  it. Backlight shorts are unproven on a 5 V anode and kept out of DIAG
  (`datasheets/PCA9955B-USAGE.md`).
- The on-screen ShiftStrip and RACE's RevBar mirror the lamp colour.
- **Bike board:** the backlight lights, so the PCA9955B answers; the lamps
  are untested. The tach wrap gave no signal through the as-built R34/C19
  filter; reworked to 10 kΩ / 47 pF, it reads a signal generator on the
  bench, not yet the bike (bringup §8).

---

## 13. Speed: wheel sensors and sensor fusion

### 13.1 Wheel speed
Front and rear Hall sensors. Settings for circumference (mm), pulses per
revolution and smoothing (ms) are under CONFIG → Wheels.

### 13.2 Fusion, stage 1 (`core/fusion/`, `docs/fusion.md`)
- A filter estimates road speed from IMU longitudinal acceleration, both
  wheels and GPS Doppler. Each source is gated, and GPS referees the wheels.
- It estimates and removes accelerometer bias.
- **`WheelCheck`.** A wheel that reads 0 for 5 s at speed is faulted into
  the health table as FrontWheel or RearWheel, and `ctrl` skips it.
- **`shown_speed_mph` / `Motion::best_mph`** (1.36) picks the speed to show:
  the front wheel, unless the filter rejected it or it is far from the fused
  speed (a wheelie, a locked front, a dying sensor). Then the fused speed,
  then the rear wheel, then GPS. Gear estimation and gear calibration use
  the same figure.
- `tools/replay` runs the same filter over a log or a synthetic ride (§30).

---

## 14. Attitude: lean, pitch, jumps

### 14.1 Lean (`core/attitude/`)
- **Lean comes from `v·Ω`, not from the accelerometer.** Ω is the world's
  yaw rate through the Euler-rate transform. The yaw gyro alone read 33° for
  a true 40°.
- Frame: x forward, y left, z up, right-handed.
- **Bar mount.** The IMU is on the handlebars. The mount offset (CONFIG →
  IMU → Roll/Pitch offset, or **Calibrate level**) rotates all six axes, and
  the steady-corner steering angle, from rake and wheelbase, is estimated
  and removed.
- **Slide detection.** On each fixtime-stamped GPS fix, the gyro's heading
  change over two fixes is compared with the GPS course change. A mismatch
  is a slide or airtime, and the v·Ω correction and the steering estimate
  hold until the two agree again.

### 14.2 Jumps (`core/jump/`)
Airtime detection: each jump's duration and landing g, the best airtime,
and the jump count for SESSION.

---

## 15. Gear indication and calibration

### 15.1 Gear estimate (`core/gear/`)
The nearest gear by the rpm-to-speed ratio, against a table in CONFIG →
Gearing (1st to 6th in rpm per mph, and the number of gears). The table
depends on the sprockets and the rolling radius, so set the wheel
circumference first. Neutral comes from the opto-isolated switch.

### 15.2 Guided calibration (`core/gearcal/Guide`)
CONFIG → Gearing → **Calibrate gears** opens a three-phase page:
1. **Intro**, with what the learner has seen so far.
2. **One gear at a time.** Ten steady seconds per gear, and the page says
   when to shift. E starts or moves on, long E skips a gear, B cancels.
3. **Results**, as the % error of each gear before you accept them.

`Window` finds steady stretches: rpm, speed and their ratio all still, GPS
agreeing, and in gear.

### 15.3 Auto-adjust (`core/gearcal/Learner`)
When **Auto-adjust** is on, the learner runs the rest of the time. It moves
one entry by at most 1 % per power cycle, after eight agreeing episodes at
least 30 s apart, and never more than 5 % from the calibrated base (CONFIG
→ Gear base). **Reset to base** puts the table back.

---

## 16. GPS

- **Driver** (`pa1616d`). Bytes only. Parsing is `core/nmea/`.
- **NMEA parser.** RMC and GGA: position, course, Doppler speed, altitude,
  satellites, HDOP, UTC and date. `day_of_year` comes from the RMC date,
  because `utc_ms` is time of day only.
- **Baud negotiation that proves itself** (1.42/1.43). The driver asks for
  57600 and checks that the receiver really answers at it before moving.
  Running at 1 Hz reports as *Degraded*, not as a fault.
- **Fix ageing** (`age_fix`). With no new fix for 2.5 s, the slot's `fix`
  goes false. One lost fix at 1 Hz is not a fault.
- **Fix timing** onto the system clock (§7.1).
- **Bike board: stuck at 1 Hz.** The receiver never hears the P4 (GPIO3 →
  U4-10), so the baud and rate commands never land; the firmware's side is
  proven (bringup §7). 9–13 satellites on the first rides.

---

## 17. Trip computer, fuel and engine hours

`core/trip/`:
- **Odometer**, saved at key-off by `power` and kept in NVS.
- **Trip A** and **Trip B.** Reset from TRIP's E or CONFIG → Trip → Reset
  trip A.
- **Engine hours**, counted while the engine is running.
- **Service reminder** (2.14, `core::trip::service_due`). CONFIG → Service →
  **Interval** (engine hours, 0 = off, the default) and **Done at**; once
  the engine has run the interval past Done at, **SERVICE DUE** shows
  (§11.1). **Mark done** (E twice) sets Done at to the hours now.
- **Setting the totals** (2.14): console `odo <miles>` and `hours <h>`, the
  laptop only (docs/console.md). Totals that failed to read back at boot
  are not saved over what is stored until both have been given.
- **Top speed.**
- **Fuel model.** Burn is interpolated between the idle rate and the WOT
  rate (gph) by load, against the tank size. It is an estimate, and there
  is no low-fuel alert (§11.1).
- **Units** (`core/units/`). Imperial or metric, and all display formatting.

---

## 18. Navigation waypoints and routing

`core/waypoint/`:
- Five named waypoints: **START, TRUCK, CAMP, FUEL, MARK.**
- **START sets itself** at the first good fix of a ride if it is not set,
  is from another day, or is more than 6 h old. A key-off at lunch keeps it.
- `logic` owns them, `keeper` saves them to NVS while stopped, and
  `logic_restore` brings them back.
- They are destinations in CRUISE → NAV's picker (§6.5).

`core/route/` (routing), `services/route_store` (the card side) and
`tools/routegraph` (the map builder): the whole design, the measured
numbers and what is left are in `docs/navigation.md`. The blackbox gains
`NavTurnFt`, `NavDestMi` and `LimitMPH` at the end (§23).

---

## 19. Performance timers

`core/perf/` (1.32), run in `logic` on every pass, off the fused speed:
- **0-30 and 0-60 mph** (0-50 and 0-100 km/h when metric).
- **60 ft, 330 ft and 1/8 mile**, with trap speed.
- **30-0 and 60-0 braking distance.**
- A run starts from a stop held for 1 s.
- Thresholds are interpolated between passes, and distance is integrated
  from speed, not from GPS.
- Session bests are kept. A new best shows a toast, and PERF's E clears the
  bests.

---

## 20. Stunt scoring

`core/stunt/` (1.32): **wheelies and stoppies.**
- **Lift** is IMU pitch minus the path's angle, asin(climb/speed). The climb
  rate comes from a small inertial + GPS-altitude filter (accelerometer
  rotated into the world frame), so a hill does not count as a wheelie. The
  climb filter is suspended in the air, so a jump does not count either.
- **Detection.** 20° for 400 ms (2.9) starts a trick, under 4° for 120 ms ends it,
  under 0.5 s scores nothing, and past 70° is a loop-out.
- **Scoring.** Points rise with angle and speed, with 1.5× in the 30–50°
  balance band. Landings are smooth (+20 %) or slammed (−30 %) by vertical
  g. Tricks within 4 s chain into a combo, +0.5× each up to 4×.
- **Grades** from POP to LEGENDARY.
- **Length.** A trick's time up and the road it covered up (`ft`,
  integrated from the fused speed), on STUNT, the toast and `_evt`.
- Session only, nothing saved. **Every threshold is a guess that has never
  seen a real wheelie.**
- Shown on STAT → STUNT and as a toast.

---

## 21. Battery and power

### 21.1 Battery monitor (`core/battery/`)
`power` feeds it at 100 Hz with the mean and the minimum VIN of each 10 ms
window, as battery volts: VIN plus CONFIG → Battery → Trim (default
+0.35 V, the input diode's drop).
- **Rest voltage** is read once per key-on, after the fuel pump's prime and
  never after a ride, since surface charge reads full.
- **Crank dip** comes from the minimum.
- **Charging voltage** comes from the mean.
- **Verdict:** good, weak, not charging, or overcharging. It feeds the
  battery alerts and DIAG → POWER. `power` owns the `battery` slot;
  `keeper`'s `elec.vbat` carries the same trimmed battery volts
  (`core::power::battery_v`).

### 21.2 Power-fail handling (`core/power/FailDetector`)
The `power` task runs at 1 kHz on core 1. When VIN falls below about 9 V and
stays down a few ms, it:
- Blanks the backlight and lamps.
- Stops `logger` from starting SD writes.
- Saves the odometer and engine hours.

Hold-up is only a few ms, so it cannot flush a file. **Never run on the
board** (bringup §8.1).

### 21.3 USB vs bike power
VIN sense tells them apart, and `ctrl` caps lamp duty on USB.

---

## 22. Health monitoring and fault recovery

`core/health/`:
- **Devices tracked:** IMU, GNSS, LED driver, pod, SD, ECU, display, BLE
  host, phone, TPMS F/R, front wheel, rear wheel.
- **One tracker, one writer.** Any task reports into the one `Tracker`
  through per-device atomics. Only `keeper` (20 Hz) turns reports into
  `DevState`s and publishes, and only on change.
- **Rules.** A device must `begin_init` before it reports. An init still
  running is not silence (1.44). A report for an absent device is ignored,
  so an ejected SD card stays ejected.
- **Retry policy.** `keeper` decides when to retry (`claim_retry`) and sets a
  bit in `Deps::retry_requests`. The task that owns the device does the
  re-init: `imu`, `gnss`, `input` (the pod bus), `ctrl` (the LED driver) and
  `render` (the panel). A re-init never runs on another core under the
  device's transactions.
- **Task watchdog.** `ctrl`, `imu`, `logic` and `render` join it and are fed
  once a loop. On the bike a 5 s stall panics, reboots with a core dump, and
  the next session's reset reason reads "task watchdog".
- **Stack watch** (`services::watch_stacks`). The tightest task's high-water
  mark goes on DIAG → System, and every task's is logged every 10 s.
- **Core dump viewer.** `keeper` reads a core dump from flash at boot, and
  DIAG shows the crashed task. Read the full dump with
  `idf.py coredump-info`.
- **Radio task demotion.** ESP-Hosted's tasks and IDF's `sys_evt` are
  lowered below priority 18 at run time (§28).

---

## 23. Logging

### 23.1 Ride log, the blackbox (`core/log/`)
- **CSV, with columns named for RaceRender**, so one file is both the
  blackbox and a video overlay source:
  `Time,Latitude,Longitude,MPH,RPM,X,Y,Lap,Heading,Gear,Lean,Pitch,GVert,LapTime,Sats,Flags,FrontMPH,RearMPH,GpsMPH,HDOP,GpsUtc,FusedMPH,FusedSigma,AltFt,HR,NavTurnFt,NavDestMi,LimitMPH,Volts,ShiftLamps,SlipPct,ThrottlePct,MapKpa,EngineTempF,IntakeTempF,InjRaw`
  (`core/src/log.cpp`). `Time` is in seconds, X is longitudinal g and Y is
  lateral g. `Volts` is the battery (0: no reading), `ShiftLamps` how many
  lamps are lit (0–8), `SlipPct` wheel slip against road speed: + the rear
  spinning, − a wheel locking (2.14). **Rules for a new column** (CLAUDE.md, *Logging is for
  RaceRender*): append it at the end, name it with its unit, never rename or
  move one, and write a missing reading as 0, never the last value held.
- **Lap tags** (`# Lap 0: 00:02:43.08`) come from the lap timer's own
  measurement and are placed between the right two rows.
- **Rate.** CONFIG → Log → Rate: No log, 1, 5, 10 (default), 25 or 50 Hz, all
  divisors of the 50 Hz tick so rows are evenly spaced. A tick that starts
  or ends a lap always gets its row, unless the rate is No log (which writes no blackbox file at all).
- **ECU columns (2.15).** `ThrottlePct`, `MapKpa`, `EngineTempF`,
  `IntakeTempF` and `InjRaw` (the injector pulse as the ECU sends it, scale
  not known) come from the K-line adapter (§35) and are all 0 on a row
  without the `Flags` ECU bit.
- **Ring.** Fixed 132-byte records, 512 of them in PSRAM, drained to SD in
  8 kB blocks. Producers never touch the filesystem or block.
- **`FlushPolicy`.** A block goes to the card when its oldest row is 2 s
  old. fsync happens 5 s after the first uncommitted write, or when the bike
  stops. A power cut loses up to about 7 s of rows.
- **File names.** `logs/sNNN.csv` until GPS gives a date, then renamed to
  `logs/2026-10-13_s007.csv`. The session number is one past the highest on
  the card, and `session_of` makes sure a number is never reused.

### 23.2 Detail log (`core/log/detail.hpp`, 1.36)
For the first rides: five more files per session in `detail/`, each from
one producer through its own ring.

| File | Rate | Producer | Contents |
|---|---|---|---|
| `_imu` | 200 Hz | `imu` | Every raw sample in sensor axes, before mount and bias; FIFO backlog per read |
| `_tach` | 50 Hz | `ctrl` | Per-window pulse intervals, extra and missed pulses, refractory rejects and how long after the spark, edge-to-ctrl and edge-to-lamp latency, tick work time |
| `_det` | 50 Hz | `logic` | Every speed source and fusion's verdict, the attitude filter's internals, stunt lift, rpm/mph |
| `_evt` | events | `logic` | Every command, health change, fix, lap, jump, trick, gear and engine change, power cut, ring drop and LED-driver fault change (`led_fault`). The file opens with the firmware, the reset reason and every setting |
| `_diag` | 1 Hz | `logger` | Heap, stacks, timing, temperatures, battery, device states, rows and drops per stream, the card's own write times |

CONFIG → Log → Detail log, on by default, about 130 MB an hour. `SdCard`
holds up to six files open. `tools/ridelog` reads a session back (§30).

### 23.3 SD card management
- **Eject** (CONFIG → SD card → Eject card) flushes and unmounts, and DIAG →
  System shows "safe to pull" until the card is removed. `storage` first has
  `logger` put its RAM blocks on the card (`Deps::log_flush_req`/`ack`,
  1.61); before that the last ~2 s of every stream, ~20 kB, were lost.
- **Bike board:** on 12 V the card mounts and writes ~32 kB/s clean; on USB
  alone it did not mount (the supply). Test the card on 12 V.
- One lock in `drivers/sdcard` guards every card call from `logger`, `ota`,
  `storage` and `wifi`.
- The SD card shares SPI2 with the IMU. The IMU's FIFO absorbs the SD
  host's bus holds.

---

## 24. Bluetooth: media control and phone notifications

`services/src/ble_service.cpp`. **On the bike board the Android app has
bonded and sent Feed lines (2026-10-31)**; no watch or iPhone has tried
there. The signal is weak (−90 dBm beside it; the phone never gets the scan
response): check the C6's antenna. The watch has run on the demo.

### 24.1 Transport, the C6, and the phone link
- **Transport.** ESP-Hosted over SDIO to the C6, with the NimBLE host on
  core 1. `start_ble` is last in the boot order, so a radio that will not
  come up delays nothing.
- **C6 firmware** (`core/c6`, `services/src/c6_firmware.cpp`). `slave_fw`
  holds `demo_p4/c6_cp`'s image (`idf.py -p /dev/ttyACM0 c6-flash`). The
  dash pushes it over SDIO when the C6 is older than 2.12 or its build id
  (first four bytes of its ELF SHA-256) differs, once per image, and never
  across an ESP-Hosted major version from a dev image. A C6 with no Wi-Fi in
  its firmware gets it replaced the same way. DIAG → Update shows
  `2.12.8 #<id>`. A C6 with **blank flash** (the first bike board's) cannot
  be reached over SDIO: flash it over its UART once (`docs/bringup.md` §9.2).
- **A lost C6 does not reboot the dash** (1.61,
  `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=n`). `ble` hears
  `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` on the default event loop (which it
  creates) and reports the radio lost until the next power-on. Console
  `c6 hold` / `c6 release` tests it.
- **NimBLE's timers mended at link time** (1.48). With
  `CONFIG_BT_NIMBLE_USE_ESP_TIMER=n`, IDF v6.0.2's FreeRTOS callouts never
  stored their event queue, so every NimBLE timer ran on the FreeRTOS timer
  task, and the central role asserted in `ble_hs_timer_exp` during sync.
  `ble_service.cpp` wraps `npl_freertos_funcs_init` (`-Wl,--wrap`, set in
  `components/services` and `demo_p4/main`). Keep both flags.
- **Names.** Advertises as `CRF250F-Dash`; the scan response carries the
  short `CRF250F` beside the ANCS solicitation, so a phone stores the bond
  under `CRF250F` and the app matches either.
- **Dash Media GATT service** (`radio-and-updates.md` §11.1): **Feed** in (phone →
  dash, line protocol) and **Ctrl** out (dash → phone: previous, play/pause,
  next).
- **Feed parser** (`core/media/`). Bounded and UTF-8-safe, with `P` as an
  anchor. Carries title, artist, album, position and duration, and
  notifications.
- **Security** (`core/ble/`). The phone must bond, and Feed needs an
  encrypted link. The first phone bonds freely, after that only in
  CONFIG → Phone → **Pairing mode**'s window. **Forget phone** clears all
  bonds. A link that is not encrypted within 30 s is dropped.
- **Notifications** (tag `N`, 1.32). Calls, missed calls and messages from
  the phone app's `NotifListener` show as a toast and on CRUISE → PHONE.
  CONFIG → Phone → Notifications and → Text moving control them. The app's
  `NotifyFilter` is JVM-tested, but the two ends have never talked over
  this tag.
- **One writer.** NimBLE's callbacks only queue, and `ble` is the one writer
  of `media`, `notify` and `ble_diag`.
- **Priority demotion.** ESP-Hosted makes its SDIO/RPC tasks at priority 23
  with no core affinity, and `ble` lowers them to 8 by name.
- The bond store writes flash only when the bike is still (§28).
- The Android companion apps are in `~/phone/media-player` and `~/phone/backpack`, not in this repo.
- **An iPhone needs no app** (1.59, `radio-and-updates.md` §11.1.1). Once it is let
  in, `ble` finds its AMS and ANCS and subscribes: what is playing, the
  transport keys (as AMS remote commands), and calls, missed calls and
  messages. The scan response solicits ANCS so the iPhone lists the dash
  under Settings → Bluetooth. Parsed in `core/media/apple`, which feeds the
  same `apply_line()`/`apply_notify()` as the feed. Never run against a real
  iPhone.

### 24.2 Heart rate from the watch (`core/hr`, 1.48–1.58)
The rider's watch over the standard Heart Rate Service (0x180D/0x2A37), as a
Garmin sends it with Broadcast Heart Rate on. Run against the rider's D2 Air
X10 on the demo (its spec sheet says ANT+, but it broadcasts over Bluetooth
too). CONFIG → Heart rate → **Watch HR**, off by default; **Max HR** sets the
zones.
- **Central role.** `ble` scans continuously while the setting is on and no
  watch is linked (3 s scans, 50 ms in 100, no controller duplicate filter),
  and `Picker` takes the strongest advertiser above −95 dBm once a scan has
  heard them all, then only that one (address or name) for the power-on.
  Turning the setting off and on forgets it. The watch is the second of
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`, on its own GAP callback, and its bond
  is never stored. The log's 30 s `watch search` line with no adverts means
  the watch is not broadcasting (its Broadcast ends when it restarts).
- **Shown** on STAT → HEART, under SPEED's rpm tile and in RACE's gear tile,
  coloured by zone, blank with the setting off (`chrome::hr_readout`); DIAG →
  Radio shows the link. The blackbox's last column is `HR` (0 for no
  reading); `_evt` has a `heart` event per link change.
- **No pulse** (1.56). Off the wrist the D2 keeps sending its last bpm with
  the contact bit clear (`04 5E` against `06 61` worn), so a reading without
  contact is published as bpm 0 (`Measurement::pulse()`). `no_pulse()` (live,
  a pulse seen on this link, readings arriving, none with a pulse) for 10 s
  raises **NO PULSE**, first of all the alerts; `_evt` logs `pulse=none`.
- **History** (1.57). `core::hr::History`, a sample every 2 s with gaps for
  no pulse, in PSRAM (`Deps::hr_history`), fed by `ui::App` every Frame so it
  fills whatever screen is up.
- **Recovery** (1.58, `core::hr::Recovery`, run by `logic`). The pulse at
  the stop less the pulse one and two minutes later, **parked only**: within
  15 s of stopping the bike must be in neutral or the engine off, and stay
  that way, after 2 min riding above 5 mph since the last one. Stops in gear
  never count. HEART's RECOVERY tile, a toast, `hr_recovery` in `_evt`.

### 24.3 Crash SOS (`core/crash`, 2.x)

After a crash the rider does not cancel, the phone texts someone the bike's
position. **The whole path has run on the bench** (2026-11-03): CONFIG →
Phone → Test crash, the countdown, `X` to the phone, the text, `K` back
(SOS TEXTED), and E's "I'm OK". The detector's thresholds have not met a
real crash or a real ride.

- **A crash is three things in order** (`core::crash::Detector`, in
  `logic` at 50 Hz): ridden upright above 10 mph within 5 s before a hit
  (so a board mounted on its side, as in the car tests, never arms); a hit
  over 3 g (the largest |a| of every 200 Hz IMU sample, `imu`'s
  `PeakHold` → `Attitude::hit_g`/`hit_ms`, so a 10 ms spike is not missed
  between logic's looks); then, within 30 s of it, the bike on its side
  (the low-passed gravity vector more than 60° off the frame's up) and under
  3 mph for 5 s straight. A jump landing is a hit with the bike upright; a
  tip-over in the car park has no speed before it; a board face-down on a
  car seat has neither. No IMU, no crash.
- **Then a 30 s countdown** in the alert box over every screen, "CRASH? SOS
  IN 27s", ahead of any alert. **Any button cancels it** and nothing is
  sent. Riding off (10 mph) cancels it too.
- **Then the SOS.** `ble` sends `X` on Ctrl with the last fix it saw
  (radio-and-updates §11.1), every 15 s until the phone answers `K`. The box
  shows SOS SENDING / SOS TEXTED / SOS OFF ON PHONE / SOS FAILED, RETRY /
  SOS: NO PHONE. **E then says "I'm OK"**: the phone texts a false alarm to
  the same contacts, only if the SOS went.
- **Two switches, both on to text.** CONFIG → Phone → Crash SOS on the dash
  (on by default; off, there is no countdown, and BIKE DOWN still shows), and
  the app's Settings → Crash SOS text (off by default) with SOS contacts
  (numbers, commas between). The app asks for SEND_SMS when it is turned on.
  Settings → Send SOS test text sends a test through the same path.
- **Bench test**: console `crash test` starts the countdown as a crash would
  (docs/console.md). With the phone linked and the app's switch on, it texts.
- The phone texts only once per SOS (its id, for 10 min: the dash's ids
  restart at power-on) and answers the resends. A text that failed (no
  signal) is tried again on the next resend.

---

## 25. Wi-Fi hotspot and web page

Built, and **run on the demo board.** On the bike the hotspot comes up and
goes down cleanly (20 cycles from CONFIG, internal RAM back each time);
nothing has joined it or uploaded. `radio-and-updates.md` §11.7.1.
- **On-demand WPA2 hotspot**, `CRF250F-Dash`, with a new eight-digit
  password each time, shown on DIAG → Update. It is never a station.
  Toggle it with CONFIG → Wi-Fi → Wi-Fi on/off.
- **Auto-off** after 10 idle minutes or when the bike moves
  (`core/wifi/`).
- **The page at `http://192.168.4.1`** has these sections:
  - **Upload firmware.** A `dash-*.bin` goes to the dev slot through
    `Deps::ota_stream` to `ota`, with the same checks as the card. An image
    built for the other board (demo vs bike) is refused. Nothing on the
    network can boot an image.
  - **Ride logs.** Download and delete, with only names the policy allows.
    **GPX export** is converted in the page's JavaScript.
  - **Lap analysis** (1.30). In-browser analysis of a log: each lap's time,
    gap and splits, two laps on a map, and speed and time lost against
    distance with the laps aligned by position.
  - **Games.** Upload a `.wad` into `games/` (`valid_game_name`), for the
    demo's Doom.
  - **Settings.** Edit every CONFIG item. Each change is a `ConfigSet` that
    `logic` applies.
  - **Firmware backup.** Download the slots' images.
- Wi-Fi is an end-user feature, not a debug channel. Debugging goes over
  USB.
- **RAM watch.** `wifi` logs free internal RAM at every change of state,
  because SDIO, lwIP and NimBLE share it.

---

## 26. Firmware updates: stable and dev slots

See `radio-and-updates.md` §11.7. **Partly run on the bike board** (2026-11-01):
`flash_dev.py` to the dev slot, Boot dev once, and a reset and a panic in
dev both falling back to stable. Install from SD, the refusals and Promote
have not run (bringup §9.1).
- **Two 6 MB app slots** hold a stable image and a dev image. Which is which
  is read off the bootloader's rollback state, never stored.
- **A dev image never confirms itself**, so any reset returns to stable.
- **Power-on always boots stable**, with no chooser.
- **DIAG → Update** has three actions, all refused unless parked:
  - **Install from SD** writes `update/dash.bin` into the dev slot.
  - **Boot dev once.**
  - **Promote to stable** works only from inside the running dev image, by
    hand.
  - Boot dev once and Promote want **E twice** (2.14): the first arms
    the row and the status line says "E again to confirm"; any other button
    cancels.
- **Other routes in.** `tools/flash_dev.py` writes the dev slot over USB,
  `idf.py flash` writes a new stable, and the Wi-Fi page uploads to dev.
- **Image header check** (`core/ota/`). An image for the wrong board is
  refused.
- **Bootloader guards** that `main.cpp` enforces at build time:
  - Rollback must be in the first bootloader flashed.
  - The partition table is at 0x10000, giving the bootloader 56 kB.
  - Secure boot and flash encryption are off by decision, because they burn
    eFuses and would end USB recovery.

---

## 27. Night dimming

`core/sun/`: sun elevation from the GPS date, time and position, and a
backlight dim curve, behind CONFIG → Display → **Dim at night**. `keeper`
sets the level, as a percentage of CONFIG → Display → Brightness, and
`ctrl` applies it.

---

## 28. Real-time guarantees

- **Tach edge to shift lamp under 3 ms.** Core 0 is a real-time island with
  no radio and no filesystem. Nothing above priority 18 is allowed on core
  0. ESP-Hosted and `sys_evt` are demoted at run time
  (`services::demote_radio_tasks`, before `ctrl` starts and once a second
  after), lwIP is pinned to core 1, and NimBLE uses FreeRTOS timers instead
  of `esp_timer` (a build error otherwise).
- **No flash writes while moving.** A flash write freezes core 0 for up to
  about 20 ms. `keeper` and the BLE bond store write only when
  `core::flash::still` holds (every speed source below the line, engine at
  most idling). OTA runs only while parked. `power` saves totals at key-off.
- **Stack discipline.** No stack frame over 1 kB, enforced by
  `-Wframe-larger-than=1024 -Werror` (`cmake/dash_stack.cmake`), with
  explicit `DASH_BIG_FRAME_BEGIN/END` exceptions.
- **No dynamic allocation after boot**, no exceptions, no RTTI.
- **IDF's `esp_timer` task is at 22 on core 0**, so NimBLE uses FreeRTOS
  timers (`CONFIG_BT_NIMBLE_USE_ESP_TIMER=n`, a build error in `main.cpp`
  otherwise). ESP-Hosted's host port still makes esp_timers.
- **Internal RAM ceiling.** Until the scheduler starts, the only internal
  heap runs from `_heap_start` to 0x4FF3AFC0 plus 18 kB; the 384 kB region
  at 0x4FF40000 is added after. ESP-Hosted's constructor once took ~87 kB
  of it and the demo boot-looped (`vApplicationGetIdleTaskMemory` assert).
  Its buffers are now in PSRAM (`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`:
  38 kB internal at constructor time instead of ~125 kB); it falls back to
  internal RAM **silently** if PSRAM fails, so check that first if the boot
  loop returns. After adding statics, `nm build/dash.elf | grep _heap_start`:
  the bike is at 0x4FF54960 (2.10; Doom is ~64 kB of it), the demo at
  0x4FF2A680 (1.40). Big buffers go in PSRAM with `EXT_RAM_BSS_ATTR` or on a
  task stack, never in a plain static. The SDIO link, lwIP and NimBLE share
  what is left at run time; `wifi` logs free internal RAM at every change.

---

## 29. The desktop simulator

`sim/`. No hardware and no ESP-IDF.

```bash
cmake -S sim -B build-sim && cmake --build build-sim
python3 sim/dash_sim.py
```

- **Runs the firmware's own code:** `ui::App`, every screen and widget,
  `logic_tick`, button debounce and `shift::pattern()`, against a fake bike
  (`sim_bike.cpp`). What the window shows is what the panel will show.
- **Sliders** for speed, rpm, lean and more. The bike reaches the speed
  slider at 0.8 g accelerating and 1.1 g braking, never in one step.
- **Five buttons** send down and up separately, so the real debounce and
  long-press timing decide what a press was.
- **Event buttons:** a jump, wheelspin and lockups, a crank, a wheelie and
  a stoppie (`cmd wheelie` / `cmd stoppie`, played as the IMU would see
  them).
- **Stand-ins** for tasks the sim does not run: OTA, storage eject, the
  Wi-Fi hotspot and a BLE phone, so every alert and every DIAG state can be
  reached.
- **Fault injection.** Each device can be set ok, failing or absent. You can
  kill a wheel sensor, lay the bike on its side, or shift the GPS clock to
  night (the window dims as `keeper` would).
- **Phone messages.** `notify <kind>\t<who>\t<text>` runs a notification
  through the firmware's parser.
- **Crash stand-in.** `crash <task|none> [new]` fakes a core dump.
- **Simulated card** in `sim_card/`, for tracks and logs.
- **Scriptable.** `build-sim/dash_sim` reads a text protocol on stdin and
  writes PPM frames on stdout, so you can pipe it into ffmpeg. The FRAME
  line carries the slip bar, the alert and the backlight.
- **`sim/wire_report`** reports bus cost per widget through the same Shadow
  as the firmware.

---

## 30. Host-side tools

| Tool | What it does |
|---|---|
| **Host tests** (`test/`, ctest) | One `test_*.cpp` per module over `core/`, `ui/` and `logic_tick`, using a 40-line harness (`test/check.hpp`); `check_no_esp_headers.sh` enforces rule 3. The fast loop |
| **`tools/pod_keys.py`** | Laptop keys 1–5 (shift for long) press the pod's buttons over the USB console; `docs/console.md` |
| **`tools/imu_view.py`** | Live pitch/lean/heading from `keeper`'s USB stream, for checking the IMU axes |
| **`tools/c6_uart.py`** | Listen to, back up, inspect and flash the C6 over its own UART (`docs/bringup.md` §9.2) |
| **`tools/replay`** | Runs `core::fusion::SpeedFilter` over a blackbox log or a synthetic ride with a known truth (`--scenario`). Tune the filter's noise and gate settings on a laptop. `plot.py` draws the output |
| **`tools/ridelog/ridelog.py`** | Reads a session off the card (blackbox plus detail log) and reports, one section per firmware assumption, what the ride measured, most suspect first, with verdicts. Optional numpy vibration spectrum and matplotlib plots |
| **`tools/flash_dev.py`** | Writes the dev slot over USB |
| **`tools/gen_font.py`** | Generates the anti-aliased font tables from Noto Sans Condensed Bold |
| **`tools/make_album_art.py`** | Converts album art into a UI image |
| **`tools/dash_media_peripheral.py`** | A laptop BLE peripheral speaking §11.1, so the phone app has a counterpart |
| **`tools/replay/plot.py`** | Draws a `replay --out` CSV as an SVG (`docs/fusion.md`) |
| **`mount/`** (CadQuery) | `pod.py`: the isolated mount — tray on four silicone damping balls in front of an H-frame clamp, three prints, tuned for fn ≈ 15–20 Hz; the frame holds the C6's RP-SMA antenna bulkhead and a zip-tie slot; the tray carries a sunshade visor over the screen (`docs/imu-vibration.md` §6). The third print is `bolt_bracket` (2026-11-09, `mount/mount-specs.txt`), not the bar clamp's cap: it bolts onto the frame's clamp half with the same four M5s, fills its half-bore, and runs an arm under the board to a plate trapped under the two handlebar-holder bolts (98.4 mm apart, raked 26.1°). That puts the panel's centre 1″ ahead of and 2″ above the bolt heads, facing 20° above horizontal. The holder bolts need 6 mm more length. `clamp()` still builds the bar cap. `mount.py`: the earlier rigid carrier. `cd mount && python pod.py` writes `mount/out/pod_*.stl`, `*.step` |
| **`kline-adapter/gen_sch.py`, `pcb_drc.py`** | Generate the K-line adapter's EasyEDA schematic/netlist from LCSC parts; check its PCB's connectivity and clearances (`kline-adapter/review.md`) |

---

## 31. The demo board build

`demo_p4/` builds the firmware for the off-the-shelf Guition P4 board, which
does exist, so UI, Wi-Fi and OTA could run before the bike board arrived,
and still can away from it.
- Its own panel and pins (`p4_panel.cpp`, `p4_pins.hpp`) and a fake bike
  task.
- Its SD card is the Guition's microSD on 4-bit SDMMC slot 0. ESP-Hosted
  owns the SDMMC host, so the card only adds slot 0.
- It reuses the bike's `ota`, `ble`, `c6_firmware`, `settings_store` and
  `wifi` services by source (`demo_p4/main/CMakeLists.txt`), so those run
  here as they are, not as copies.
- The serial command `inputs` traces raw pod pins, touch zones and decoded
  presses for 30 s.
- `c6_cp/` is the C6 co-processor firmware (BLE + Wi-Fi) that `slave_fw`
  holds and the dash pushes to the C6. `demo_p4/README.md` has the build and
  the demo's own bring-up story.

### 31.1 Doom (`components/doom`, both boards)
1.32 on the demo; on the bike since 1.62, **parked only**. DIAG's menu has a
ninth row, `doom::Program` (a `ui::IProgram`), over doomgeneric
(`components/doomgeneric`, GPL-2.0, vendored). Each board's main supplies a
`doom::Board`: how a picture reaches its panel, held buttons, parked.
- E starts it on render's task, and render draws nothing while `active()`.
  The bike shows 320×200 at 1.5×, 480×300; its Doom task is core 1,
  priority 1.
- **Leaving needs no reset.** Quit Game pauses it (`I_Quit` → `DG_Quit`) and
  DIAG → Doom resumes where it was; on the bike so does moving
  (`core::flash::still` false; it starts only still). Every `exit()` in Doom
  is `DG_Exit` (`src/dg_dash.h`), which stops Doom for the power-on
  ("error") and gives the panel back.
- **Buttons are raw held state** (`Deps::buttons_held`, the pod before
  debounce, ORed with the console's `hold <mask>`): A/B forward/back, C/D
  turn, E fire/select/yes, A+B menu, C+D use. `pod_keys.py` plays it only on
  a terminal that reports key releases (kitty protocol); elsewhere it sends
  decoded presses, which Doom never sees (`docs/console.md`).
- The WAD is the first `.wad` in `games/` on the card, uploaded from the
  Wi-Fi page's Games section (`Deps::games`, `core::wifi::valid_game_name`).
- **Memory.** The big arrays are in PSRAM by `DG_EXTRAM` (IDF v6.0.2's
  ldgen emits nothing for a `* (extram_bss)` mapping) and `states` is read
  from flash; it still costs ~64 kB of internal RAM on either board.

---

## 32. Release and version management

- `firmware.txt` holds the version and a one-to-three-word feature name.
  Bump it before a build you mean to keep. `cmake/dash_version.cmake` makes
  the image's version string `"<version> <board> <feature>"` (31 chars max).
- Every build of either board is copied to `releases/` as
  `dash-<version>-<date>-<board>-<feature>.bin`, read back out of the image.
  `releases/` is in git.
- The Update screen and the Wi-Fi page show the version and date for both
  slots. An upload built for the other board (`demo` vs `bike`) is refused.
- C6 images are `releases/c6-<esp_hosted version>-<date>-<feature>.bin`;
  `releases/README.md` lists them and the known-bad dash images.

---

## 33. Deliberately not here

- **A wire to the ECU.** The board has no CAN transceiver and no spare
  pins. ECU data comes by radio from a separate adapter (§35), and without
  one its fields render `--`.
- **TPMS.** The observer scan waits on capturing a real sensor. The phone
  half is written.
- **USB mass storage.** The ownership seam exists, and the TinyUSB half is
  not written. It must unmount the card first or the FAT is corrupted.
- **A tuned lap delta.** The matcher is a placeholder (§7.3).
- **A track database** beyond the 16 saved finish lines.
- **Low-fuel alert.** The fuel model is a guess.

---

## 34. Configuration reference

Every item in `components/core/src/config.cpp`, by CONFIG submenu.

| Group | Item | Range / options | Unit |
|---|---|---|---|
| Display | Brightness | 5–100, step 5 | % |
| Display | Units | Imperial / Metric | — |
| Display | UTC offset | −12 to +14 | h |
| Display | Dim at night | On / Off | — |
| Display | Speedo | Wheels / Fused | — |
| Display | Slip bar | On / Off | — |
| Display | Popups | On / Off | — |
| Display | 12h clock | On / Off | — |
| Display | Palette | Standard / Contrast (2.11): Contrast takes the background to true black and the panels darker, and brightens the captions and rules, for a washed-out panel. Mapped in `FrameBufferCanvas` by `ui::theme::palette_color`; screens never see it | — |
| Startup | Gauge sweep | On / Off | — |
| Startup | Sweep time | 500–5000, step 100 | ms |
| Startup | Splash | On / Off | — |
| Wheels | Front circ. | 1000–3000, step 5 | mm |
| Wheels | Rear circ. | 1000–3000, step 5 | mm |
| Wheels | Front pulses | 1–32 | /rev |
| Wheels | Rear pulses | 1–32 | /rev |
| Wheels | Smoothing | 0–100, step 5 | ms |
| Shift | First lamp | 2000–12000, step 100 | rpm |
| Shift | Span | 200–6000, step 100 | rpm |
| Shift | Hysteresis | 0–500, step 10 | rpm |
| Shift | Flash above | 3000–14000, step 100 | rpm |
| Shift | Brightness | 5–100, step 5 | % |
| Shift | Revs/spark | 1–2 | — |
| Shift | Lead time | 0–300, step 10 | ms |
| Shift | Peak hold | 0–5000, step 250 | ms |
| Shift | Tach smooth | 0–50 | ms |
| Lap | Line width | 10–200, step 5 | ft |
| Lap | Arm speed | 0–60 | mph |
| Lap | Min lap | 2–600 | s |
| Lap | Set line here | action | — |
| Lap | Clear laps | action | — |
| Lap | Forget track | action | — |
| Gearing | Gears | 1–6 | — |
| Gearing | 1st … 6th | 40–900 (6th from 0), step 5 | rpm/mph |
| Gearing | Calibrate gears | action | — |
| Gearing | Auto-adjust | On / Off | — |
| Gearing | Reset to base | action | — |
| Gear base | Base 1st … Base 6th | 40–900 (6th from 0), step 5 | rpm/mph |
| Fuel | Idle burn | 0.02–2.0 | gph |
| Fuel | WOT burn | 0.10–8.0 | gph |
| Fuel | Tank | 0.20–10.0 | gal |
| IMU | Roll offset | −45 to +45, step 0.5 | deg |
| IMU | Pitch offset | −89 to +89, step 0.5 | deg |
| IMU | Calibrate level | action | — |
| Trip | Reset trip A | action | — |
| Phone | Forget phone | action | — |
| Phone | Pairing mode | action | — |
| Phone | Notifications | On / Off | — |
| Phone | Text moving | On / Off | — |
| Phone | Crash SOS | On / Off (on by default) | — |
| Heart rate | Watch HR | On / Off (off by default) | — |
| Heart rate | Max HR | 120–230 | bpm |
| Nav | Avoid dirt | On / Off (off by default; NAV's picker switches it too) | — |
| SD card | Eject card | action | — |
| SD card | USB drive | action: the card as a USB drive on a laptop, parked only (a toast says so when moving); Eject on the laptop gives it back and the dash restarts (2.6) | — |
| Wi-Fi | Wi-Fi on/off | action | — |
| Battery | Trim | −1.0 to +1.5, step 0.05 | V |
| Log | Rate | No log / 1 / 5 / 10 / 25 / 50 Hz | — |
| Log | Detail log | On / Off | — |
| Diag | FPS counter | On / Off (off by default) | — |
| Diag | Frame rate | 10 / 20 / 30 / 60 / 120 (default) Hz / Unlimited | — |
| Service | Interval | 0 (off, default)–200 | h |
| Service | Done at | 0–100000 | h |
| Service | Mark done | action (E twice) | — |
| ECU | Adapter | On / Off (off by default) | — |

### Commands
Everything a screen, the Wi-Fi page or a service can ask of the owning
tasks (`core/state/command.hpp`): LapSetLine, LapArm, LapClear, TripReset,
ConfigSet, LogStart, LogStop, StorageToHost, StorageEject, ImuCalibrate,
ImuSetMountOffset, MediaCtrl, BleForgetPhone, BlePairingMode, OtaInstallSd,
OtaBootDevOnce, OtaPromote, WifiToggle, NavDropWaypoint, NavClearWaypoint,
GearCalStart, GearCalNext, GearCalSkip, GearCalCancel, GearCalReset,
LeanPeaksReset, TrackForget, PerfReset, StuntReset, CrashCancel, CrashTest,
the Nav commands, ShiftLampTest, ServiceDone, OdoSet, EngHoursSet.

## 35. The ECU link (K-line adapter)

*Since 2.15. Host-tested; **nothing here has run against an adapter**, and
the adapter's board is not ordered (`kline-adapter/review.md`).*

The ECU's live data reaches the dash by radio. A separate board under the
seat reads the Keihin ECM's K-line and broadcasts each table it reads over
ESP-NOW; the dash's C6 hears it and passes it to the P4. Nothing is sent
back to the adapter. The adapter's own firmware is not in this repo yet.

- **Turning it on.** CONFIG → ECU → Adapter, off by default. On, the C6's
  Wi-Fi runs all the time as a station that joins nothing, on channel 6,
  so that it can listen; the update hotspot uses the same channel and comes
  up beside it. Off, the radio is BLE's alone, as before.
- **What it gives.** Throttle opening, the ECU's engine and intake
  temperatures, manifold pressure and the raw injector pulse: STAT →
  ENGINE's throttle tile and top line, and five blackbox columns (§23).
  The ECU's rpm is not used: the pickup's is first-hand, and the shift
  lamps never wait on a radio.
- **The frame** (`core/ecu`): magic `0x4B`, version, sequence number,
  flags (bit 0: the ECU is answering), the Keihin table number, a length,
  then the table's bytes as the ECU sent them. A frame with no table is a
  heartbeat. The dash decodes table `0x11`; other tables are accepted and
  ignored.
- **Which adapter.** The first one heard after power-on, for that power-on.
  Frames from any other are counted and dropped. There is no pairing and
  no encryption.
- **Stale and lost.** A reading older than 500 ms is no reading, and every
  ECU field goes back to `--` and 0 in the log. Frames are lost (the radio
  is shared with BLE); they are counted by sequence number and nothing
  needs every one.
- **The byte map is a guess.** It is community reverse-engineering from a
  CRF250L, a CBR600RR and a Winner 150, not a CRF250F capture. Throttle is
  scaled ×0.5 on the weaker of two readings of the sources. Check every
  field against the bike before believing a column.
- **Seeing it work.** The console summary's `ecu` line says whether the
  adapter is heard and the ECU answering, with frames lost and refused;
  the log prints the adapter's address when it is first heard.

Code: `core/ecu` (format, decode, link state; `test/test_ecu.cpp`),
`services/ecu_link` (`EspNowEngine`), `services/wifi_service.cpp` (the
listening station), `demo_p4/c6_cp/main/dash_espnow.c` (the C6's half).
