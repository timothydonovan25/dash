# Bring-up

§17 of the architecture document is a *build order* — what to write, in
what sequence. This is the other half: when the board is on the bench,
what to do, what you should see, and what each failure means.

This is for the **P4 board**: the Guition JC-ESP32P4-M3-C6 module (U16),
the panel on an 8-bit 8080 bus, the shift lamps and backlight on a
PCA9955B (U2). `p4-board-pin-map.md` is the pin truth and its §6 is the
pre-order audit; read §6 before starting here.

Every driver was written against a datasheet and a netlist, which is not
the same as correct. The places that can only be settled here are marked
`BRINGUP` in the source and collected in §10 below.

**The first board arrived on 2026-10-28.** Where it has got to:

| § | Status | Notes |
|---|---|---|
| 0–1 Before power, rails | Done | Rails in spec |
| 2 Boots | Done | P4 silicon **v3.2**, so the build is for v3.1+, at 400 MHz. Boot time and the watchdog test not done |
| 3 Panel | Done (HT035) | IPS init, mirrored on both axes, 25 MHz write clock. The VIEWE UEED035HV probe is not run |
| 4 DIAG → Sensors, Bus | Not done | |
| 5 Pod, neutral | Neutral done; pod on the demo only | Neutral works on the bike. The pod works on the demo; its GH1.25 connector for this board is not fitted yet. Until it is, `tools/pod_keys.py` presses the buttons from the laptop over USB (§5) |
| 6 IMU | Bench only | SPI2 deadlock with the SD card found and fixed; axes remapped for the upright board, checked with `tools/imu_view.py`. The riding check (step 7) is not done. The ride logs: mount roll/pitch never set (gravity implies ~10° roll, and it differs between sessions), gaps behind SD writes, and the chip's own clock says ~213 Hz, not 200, while by the P4's clock samples arrive every 5.3 ms (~189 Hz: the part's oscillator is ~6 % slow), so `icm42688p.cpp` back-dates a batch 5 ms a sample and is up to ~5 ms out at its oldest (the report's p99 8 ms); totals stay right, as each batch is anchored at now. Once (2026-10-31, after the SD card was swapped with the board powered) it answered WHO_AM_I 0xFF through every reset and retry until a full power cycle: a USB or software reset does not power the IMU down. Likely its I2C side, on by default, latching onto the card's traffic on the shared SCK/MOSI; 1.60 turns I2C off first thing after the soft reset (`INTF_CONFIG0` = 0x33). Not yet re-tested with a hot-plugged card |
| 7 GPS | Fixes, at 1 Hz: **P4 → GPS TX dead** | 9–13 satellites on the first ride. A cold receiver's placeholder date is now ignored. Stuck at 1 Hz because **the receiver hears nothing the P4 sends**: it ignores even a command at the 9600 it is talking at, and keeps its default sentences. The firmware's side is proven (2026-11-01, IO dump): GPIO3 drives UART1 TX. Check U16-6 → U4-10 (§7) |
| 8 Tach, wheels, lamps | Not working / not done | Tach wrap fitted, **no signal** (the filter, §8). Wheel sensors not fitted. Backlight lights, so the PCA9955B answers; the lamps are untested. On the first rides (s156, s157, 2026-10-29) the speedo read 0 while GPS saw up to 50 mph: a **fusion deadlock**, fixed in 1.60 (`docs/fusion.md`, replayed over both rides; not yet ridden). The absent wheel sensors read 0 and are always offered and accepted, so the filter grows sure of 0, gates out the first fix at ~20 mph, and GPS goes suspect; the all-suspect restart never fires because the wheels are never suspect. The board was also not held rigidly (up to 15 g and 1100 dps; gravity 60° apart between parked spells), so lean and pitch from those rides are meaningless |
| 8.1 Power-fail | Not done | |
| 9 SD | Done on 12 V | Mounts and writes ~32 kB/s with no errors; on USB alone it did not mount |
| 9 Radio | Phone linked | The C6 came with **blank flash**; flashed over its UART 2026-10-31 (§9). SDIO 40 MHz 4-bit, ESP-Hosted 2.12.8, `slave_fw` current, advertising as CRF250F-Dash. The Android app bonded and sent Feed lines (2026-10-31). Signal is weak: the laptop hears it at −90 dBm beside the board and the phone never gets the scan response, so **check the antenna is on the IPEX**. No watch, iPhone or nRF Connect tried. The Wi-Fi hotspot comes up (2026-11-01: 20 on/off cycles from CONFIG, internal RAM back to 301 kB each time, 9 kB kept by the first start; off on 10 idle minutes); no phone has joined it or uploaded |
| 9.1 OTA slots | Steps 1, 4, 5, 6 done | 2026-11-01: `flash_dev.py` to ota_1 with stable untouched; Boot dev once from DIAG → Update ran ota_1 as dev; a reset, and a panic in dev (console `panic`), both came back stable on ota_0 with no input; the panic's core dump decoded with `idf.py coredump-info` (needs GDB on PATH, Build). Not done: install from SD, the refusals, Promote, a card pulled mid-install |

**The order matters more than the speed.** Each session below assumes the
one above it works. Skipping ahead means debugging two unknowns at once,
which is how a week goes.

---

## 0. Before you apply power

**Check FPC pins against the panel's own datasheet.** The FPC1 table in
`p4-board-pin-map.md` §4 is from the HT035SHV007NS datasheet. If your
panel is a different one, compare every pin, especially IM0–IM2 (FPC
38–40, wired as 011 for 8-bit 8080) and the supply pins. Powering a
mismatched panel can destroy it. Do this before anything else.

**Set the partition table before the first flash.** Changing it later
erases NVS, and NVS holds your odometer. `partitions.csv` is already in
the tree and `sdkconfig.defaults` points at it; just confirm it is what
you want before the first `idf.py flash`. The table sits at 0x10000, not
IDF's 0x8000, to give the bootloader room (it is never updated over the
air); `main.cpp` will not build with a stale `sdkconfig` that says 0x8000.
Secure boot and flash encryption stay off: see `sdkconfig.defaults`.

**Rollback has to be in the first bootloader.** Stable/dev firmware
(architecture §11.7) depends on `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, and
the bootloader is never updated over the air. `main.cpp` refuses to build
without it; if it errors, delete `sdkconfig` and rebuild (§11).

**Fit the fuse.** F1 is only a holder; the BOM has no fuse. A 1 A fuse
goes in it. Without one the board runs from USB and is dead from the
bike, which looks like a supply fault.

**Know your bench current budget.** On USB, VUSB feeds VIN through D5, so
VIN sits around 4.3–4.6 V — at the buck's 4.5 V minimum, with the
backlight LDO in dropout. Eight lamps at 30 mA plus the backlight plus the
P4 is close to a 500 mA port. `ctrl` caps the lamps at
`bsp::kShiftDutyUsbMax` when VIN sense says the supply is USB. If the
board browns out when the lamps come on on the bench, that is what it is.
A bench supply on the bike input (12–14 V) removes the doubt.

---

## 1. Rails, before the modules go on

U16 (the P4 module) and U4 (the PA1616D) are not in the BOM; they are
hand-fitted after assembly, along with the bottom-side shift LEDs
(LED0–LED7) and any through-hole part not assembled (SW2, U8, U11). That is an opportunity: **check the rails
with neither fitted**, when a wrong voltage costs a resistor rather than
a module.

With 12 V on U8 and SW2 on, measure:

| Net | Expect | If not |
|---|---|---|
| VIN | about 11.6 V (12 V less D6) | SW2, D6 orientation, the fuse |
| VCC | **3.31 V** | U3, its feedback divider R4/R5, L1 |
| LEDA | about 5 V | U5. Pin 3 (GND) is on no net; the part is grounded through its tab only |
| `VIN_SENSE` (U16-21 pad) | about 2.1 V at 11.6 V VIN | R17/R18, D7 |
| `ESP_EN` (U16-75 pad) | VCC, and to 0 V with SW3 held | R27, C30 |

Then on USB alone: VIN about 4.3–4.6 V, and VCC should still read 3.3 V
at this near-zero load. If VCC is far off at this point, nothing later in
this document will work.

Fit U16, then U4. Re-check VCC after each.

---

## 2. It boots at all

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 flash monitor
```

Flashing, the console and JTAG all go over the USB-C connector, which is
the P4's USB-Serial/JTAG on GPIO24/25 (U16-42/43). UART0 is also on test
pads H8/H9 (U16-56/57) as a fallback.

**Read the chip revision first.** `esptool.py chip_id` prints it. The
first board's module is **v3.2**, and `sdkconfig.defaults` builds for
v3.1+ (`CONFIG_ESP32P4_REV_MIN_301`), at 400 MHz since v3 has no 360. The
demo board is v1.3 and `demo_p4/sdkconfig.defaults` keeps the pre-v3
selection; an image built for one does not run on the other. A module of
another revision means changing that line (§11 explains why editing the
defaults alone is not enough).

**What you should see.** `dash v2 — N config items`, then thirteen tasks
starting (`ctrl`, `imu`, `gnss`, `logic`, `input` on core 0; `render`,
`logger`, `storage`, `keeper`, `power`, `ota`, `ble`, `wifi` on core 1),
then `running`.
`wifi` is silent until the hotspot is turned on; when it is, check its
`up: internal free` line before uploading anything (architecture §11.7.1).
`ota: running <version> from ota_0, stable; other slot empty` says the
rollback bookkeeping is reading the bootloader's state. If the config
table is malformed it says so here rather than weeks later as a setting
that will not save.

**If it reboots in a loop**, read the coredump —
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` is already on. A panic before the
first log line is almost always PSRAM, the chip revision, or the
partition table, not your code. If PSRAM is the suspect, turn
`CONFIG_SPIRAM_MEMTEST` back on for a build: it is off so the boot does not
pattern-test all 32 MB every time (§11 for how to change it).

**Boot time.** The order is built for it: the LED driver, tach and VIN
come up and `ctrl` starts before the panel is touched, then the panel, IMU
and pod, then the other tasks. The GPS (up to a few seconds of baud negotiation)
and the SD mount happen on `gnss` and `storage`, after the dash is already
drawing. A power-on also skips the bootloader's hash of the image
(`CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`); a software restart still
checks it. The estimate is about half a second from the switch to lamps
and screen, never measured: the log's millisecond stamps from the first
`I (` line to `main: running` are the number to write down here, and a
scope on 12 V against the backlight gives the rails as well.

**The watchdog, once, on purpose.** `ctrl`, `imu`, `logic` and `render`
join the task watchdog and the bike panics on it (5 s). Build once with an
endless loop at the top of `logic`'s loop, behind a flag, and check:
the board reboots about 5 s after the hang; the next session's `_evt` says
`task watchdog`; DIAG → System reads **CRASHED in logic** and the DIAG
menu says CRASHED; `idf.py coredump-info` names the loop. Time it from the
hang to the dash drawing again. Writing the core dump erases up to 116 kB
of flash before the reboot, so this is also the number for how long a
crash on a ride leaves the dash dark. Clear the dump afterwards:
`parttool.py --port /dev/ttyACM0 erase_partition --partition-name=coredump`.

**The console with nobody listening.** On the bike the USB-Serial/JTAG
console has no host. Unplug USB (run from the bike supply), and compare
`ctrl` overruns and the `_diag` timings with and without the Wi-Fi
hotspot toggling, which logs at every change. If a log line blocks for its
timeout each time, lower `CONFIG_LOG_DEFAULT_LEVEL` for bike builds.

**Optimisation.** The build is `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
If a backtrace is unreadable during bring-up, a `-Og` build
(`CONFIG_COMPILER_OPTIMIZATION_DEBUG`) reads better; go back before riding,
so the detail log's timings are the build that rides.

**QIO flash, on the demo board first.** Both projects flash in DIO at
80 MHz. QIO roughly halves a cache refill from flash (`render` and the
350 kB font notice). It needs the module's flash to have its QE bit set:
try `CONFIG_ESPTOOLPY_FLASHMODE_QIO` on the demo, boot it cold several
times, and only then on the bike.

**If nothing enumerates on USB**, hold BOOT (SW1, GPIO35) and tap reset
(SW3) for download mode. GPIO36 must be high for that, which R20 does.

At this point nothing is on the screen and that is expected.

---

## 3. The panel

Build-order step 2. The panel is on an 8-bit 8080 bus driven by the P4's
LCD_CAM: D0–D7 on GPIO33 down to GPIO26 (the bus is reversed as drawn), plus
four control pins.

**The control pins are where the board drew them**, not where the
first pin map planned: U16-73 RST (GPIO53), 72 WR (GPIO52, via R30), 71 DC
(GPIO51), 70 CS (GPIO50), 69 TE (GPIO49). `pins.hpp` follows the board. If
the panel stays white, confirm with a scope that WR toggles on the
connector during a blit before suspecting anything else.

**The connector is top-contact and the panel's FPC arrives flipped.** The
panel sits on the bottom side and its tail wraps round the right edge into
FPC1 (AFC07, slide lock) with the gold fingers **up**. Panel pin 1 is at
the top end. Pull the slider out, insert fingers-up, push it home.

`TFT_RST` is a plain GPIO with R3 holding it high, and CS has R21 — there
is no expander in front of the panel any more, so the panel can be
brought up on its own.

The backlight is **not** a GPIO: LEDK1–3 are PCA9955B outputs OUT8–10,
with LEDA on U5's 5 V. No PCA9955B, no backlight — so a black screen can
be an I2C fault (§4).

**What you should see.** The boot screen, then the SPEED screen drawing.

| Symptom | Where to look |
|---|---|
| Backlight on, white screen | Panel got power but no init. WR, DC, RST, CS — check each toggles. |
| Backlight off entirely | The PCA9955B on I2C1 (§4), then LEDA at about 5 V, then FPC 33–36. |
| Image mirrored or upside down | `ili9488.cpp`, `swap_xy` and the mirror after it. The first board's HT035 needed `swap_xy` and a mirror on both axes, as fitted. `panel_test`'s corners screen shows which. |
| A negative image, or a fine crawling speckle at every clock | The init sequence, not the bus: the HT035 is IPS and needs `esp_lcd_new_panel_ili9488_ips`. The default (TN) sequence did exactly this on the first board. |
| Torn, speckled, or wrong colours | Drop `bsp::kTftWrHz` (25 MHz) before suspecting anything else. Swapped red/blue is the byte order, not wiring. |
| Colours right, garbage in places | A data line: D0–D7 are panel pins 17–24, which land on connector pads 24–17, from GPIO33 down to GPIO26. |

The panel runs **RGB565, two bytes per pixel**, on this bus: 30.7 ms per
full frame at 10 MHz, about 15 MHz in spec, about 12 ms at the 25 MHz the
first board runs. `render` runs at 120 Hz and
sends only what changed, and only where it differs from the shadow copy:
SPEED's riding frame is about 1 ms on the wire, against an 8.3 ms period.
**Raise the write clock** once the panel is right: `kTftWrHz` in steps
(12.5, 15, 20 MHz), `panel_test` and a ride's worth of screens at each,
and keep the last one with no speckle, wrong colour or torn band. Every
blit and the 31 ms full frame shrink in proportion. **Done on the first
board:** clean at 25 and 30 MHz with the IPS init, held at 25. If a panel
speckles hot or on the bike, lower it first. If you find yourself
writing panel command sequences, stop — that is the `esp_lcd_ili9488`
managed component's job.

---

## 4. DIAG → Sensors, and DIAG → Bus

**This is the week these screens were built for.** Get here early.

DIAG → Sensors is one row per device: `absent` / `init` / `ok` /
`degraded` / `FAULT`. Work down it. Anything `absent` is either genuinely
not fitted (ECU and TPMS on this revision; Phone until one connects) or
never reported at all.

DIAG → Bus answers a different and more useful question: *is the device
dead, or is the bus dead?* Four buses to tell apart:

| Bus | On it | Notes |
|---|---|---|
| SPI2 | IMU (U1), SD card | "imu FAULT" with "sd ok" is the part; both faulted is the bus |
| I2C0 | the handlebar pod, 100 kHz | over the cable; see §5 |
| I2C1 | PCA9955B (U2) at 0x15, 1 MHz | on-board; lamps and backlight |
| UART1 | PA1616D | see §7 |

The PCA9955B's reset pin is on `ESP_EN`, so it resets with the P4 but
**not** on a software reset or a reset over USB-JTAG. The driver does not
rely on that: `init()` blanks the PWM registers first, then rewrites the
mode and current registers. If the lamps come up in a strange state after
a reflash, that init sequence is the suspect.

Two rows on that screen only matter once something has gone wrong:

- **I2C recover** — how many times the pod's bus was clocked free.
- **still stuck** — how many of those failed to release SDA. Non-zero
  means a wiring fault, not a glitch. A pod that browns out and recovers
  increments the first and not the second.

---

## 5. The handlebar pod

**Status:** the pod works on the demo board; on the bike board it is
untested, as its GH1.25 connector is not fitted to the pod's cable yet.

**Without the pod**, press the buttons from the laptop:
`python3 ~/dash/tools/pod_keys.py` (after `export.sh`, for pyserial). Keys
1–5 are a short A–E, shift+1–5 a long one, q quits; the board's log scrolls
alongside. It sends `btn X` / `long X` lines over the USB console to
`main/serial_buttons.cpp`, which posts them into the input queue after
debounce, so the screens cannot tell them from the pod, but debounce, hold
timing and repeat are not exercised.

**100 kHz, not 400.** Already set in `bsp/buses.cpp` with the reason in a
comment. 4.7 k pull-ups (R35/R36) over half a metre give ~470 ns of rise
time; fast mode allows 300 ns. At 400 kHz this works on the bench and
fails in the rain, which is the worst possible failure mode.

**There is no interrupt line on CN1.** The pod is polled at 200 Hz. Do not
go looking for an INT pin. CN1 is 1 SDA, 2 SCL, 3 3.3 V (through R31 and
the F2 polyfuse), 4 GND — check the pod's harness against that.

| Symptom | Meaning |
|---|---|
| All five buttons read pressed | IPOL inversion, or the pod came back from a brown-out with default registers. `refresh_config()` exists for exactly this. |
| Nothing responds, ever | Bus wedged, or SDA/SCL swapped in the harness. Watch **I2C recover** climb on DIAG → Bus. |
| Buttons work, then stop after a jump | Brown-out. Expected; recovery should handle it. If **still stuck** climbs too, it is the cable. |
| Pod dead, F2 hot | A short in the cable; the polyfuse is doing its job. |
| `input: pod not answering: probing it once a second` | Three bus recoveries in a row did not bring it back (unplugged, or not fitted): `input` stops the 200 Hz reads, each a 10 ms timeout, and tries once a second. `pod answering again` when it does. A press is lost for up to a second after plugging it in. |

**Neutral** is separate, and works on the first board: GPIO12 (U16-19), opto-isolated by U6 and pulled
up by R64, and it reads **LOW when in neutral**. A neutral light that reads
inverted is this, not a bug. Before connecting it to the bike, measure
the stock neutral wire: the dash pulls it to about 12 V through R6 when
in gear (`p4-board-pin-map.md` §6.2).

---

## 6. The IMU

Build-order step 5. **The axis mapping is checked on the bench only**
(`icm42688p.cpp:202`, BRINGUP). The board stands upright, LED edge on top,
facing the rider: forward is chip +Z, up is chip +Y, left is chip +X.
`tools/imu_view.py` (fed by `keeper` over USB) showed pitch, lean and
heading moving the right way on the first board; step 7's ride has not
been done. The data-ready interrupt setup (`icm42688p.cpp:137`) is still
from the register map only: if it is wrong the service falls
back to polling, and DIAG → System counts how often.

Do this statically before trusting any filter output:

1. Bike upright and still. `g_vert` ≈ 1.0, `g_lat` ≈ 0, lean ≈ 0.
2. Lean it to the right. Lean angle should go **positive**, and the marker
   on the speed screen's gauge should move right.
3. Lean it left. Negative.
4. Brake hard by hand (rock it forward). `g_lon` sign.
5. Lift the front, quickly. Pitch goes **positive** nose up, straight
   away. If it dips first or creeps up a second late, the accelerometer
   is right and the pitch gyro's sign is not (gy < 0 nose up).
6. Turn the bars to each lock. Lean moves about 0.45° per degree of steer,
   the wrong way to the bars. That is the bar mount, not a fault; the
   filter can only take it out while moving.

The frame is x forward, y **left**, z up, right-handed (`attitude.hpp`).
The stand does not test the yaw gyro: the `v·ω` reference only takes over
above 5 mph. So:

7. Ride a slow right-hand circle, 10–15 mph. YAW on DIAG → IMU is
   **positive**, lean is positive and steady. If yaw is negative, or lean
   reads right on the stand but left in the circle, gz is the wrong sign —
   and if the fix is to negate gz alone, a second axis is wrong too, since
   a right-handed part cannot need one axis negated.

Fix the mapping in the driver, not in `core/attitude` — the filter's
convention is documented and the rest of the code depends on it.

Only then tune the filter against a bench tilt. Lean comes from `v·ω`, not
from the accelerometer, so it will read nonsense while stationary and that
is correct. Check `Config::rake_deg` and `wheelbase_ft` against the bike:
they size the steering correction.

**The part's own timestamp.** Each `_imu` row carries the chip's 16-bit
timestamp (`ChipTs`) and the packet header (`Hdr`), and `ridelog` §6 fits
the firmware's back-dated times against them. Check `Hdr` first: bits 3:2
should be `10` (an ODR timestamp), which is 104 decimal in a full
accel+gyro+temp packet. If they are `00`, the FIFO carries no timestamp
and TMST_CONFIG / FIFO_CONFIG1's FIFO_TMST_FSYNC_EN need setting
(`icm42688p.cpp`, from the datasheet). Then read the real ODR, the drift
between the two clocks, and the back-dating's p99 error, overall and after
a stall.

**Gyro bias against temperature.** Leave the dash on, parked, engine off,
from a cold start for 20–30 minutes (sun on it, if possible). Average
`Gx`/`Gy`/`Gz` in `_imu` over a few seconds at a time and fit each against
`TempC` (`ridelog` does not do this yet). The datasheet's typical drift is
about 0.005 °/s per °C; your part's slope and how well temperature
explains it decide whether a correction is worth writing.

The IMU shares SPI2 with the SD card, and the IDF's SD host holds the bus
for a whole SD command, busy time included (architecture §5.1). If IMU
samples arrive in bursts while logging, that is SD write-busy time; the
FIFO absorbs it, and `Drain` > 0 in `_imu` counts it. If the IMU
faults intermittently at 24 MHz, drop `bsp::kSpiHzImu` toward 10 MHz —
the bus is about 90 mm long with no source termination.

---

## 7. GPS

Build-order step 6. Take it outside. **First board:** fixes with 9–13
satellites. A cold PA1616D sends the placeholder date 050180 (2080-01-05)
before it knows; the date is now taken only from a valid fix, after the
first boot with a card renamed a session to `2080-01-05`. **Every power-up is a cold start**:
VBACKUP (U4-4) is tied to VCC, so the module keeps nothing when the dash
is off. Expect minutes, not seconds.

The module boots at 9600 baud and 1 Hz every time (VBACKUP is VCC, so it
keeps no setting), and the driver moves it to 57 600 and 10 Hz
(`kGpsBootBaud`, `kGpsRunBaud`). It sends the baud command at both rates,
then listens for a sentence that passes its checksum, at 57 600 and then
9600, up to 1.5 s each; a receiver heard at neither is a fault. Heard
only at 9600, it is asked once more at 9600 (the first ask can go out
before the receiver has booted) and listened for at 57 600 again. Then
RMC+GGA only, SBAS off (the datasheet allows SBAS only at 5 Hz or slower)
and 10 Hz, each confirmed by the receiver's `PMTK001` answer. **DIAG → Bus
reading `UART1 gps  1 Hz`** means it works but stayed slow: still at 9600
(it never took the baud command), or it refused 10 Hz. The health table
calls that Degraded, so the DIAG menu shows it; GPS data is still used. A
P4-only restart (the watchdog) finds it already at 57 600 and must come
back at 10 Hz: check that once.

DIAG → GPS shows sentence count and bad-checksum count. **Sentences
climbing with checksums climbing** is a baud or wiring problem.
**Neither climbing** is nothing arriving at all: U4-9 (module TX) goes to
GPIO2, U4-10 to GPIO3. The parser is host-tested against captured
sentences, so a parse bug is unlikely — the suspicion should fall on
UART1 first. The module's output is 2.8 V logic against the P4's ~2.5 V
input threshold: workable, but a marginal edge looks like checksum
errors.

**The UART is interrupt-driven.** `gnss` blocks in the driver and wakes
when bytes arrive; UART1 hands bytes on after 8 in its FIFO or 2 idle
characters (`kGpsRxFullBytes`, `kGpsRxIdleChars`), so a fix is stamped
within ~1.5 ms of its sentence. Check that bad checksums do not climb any
faster than they did (they would if the FIFO overran), and that `ctrl`
overruns stay at 0 with the GPS streaming: the UART interrupt is on core 0,
a couple of hundred times a second.

**First board: the receiver never hears the P4** (2026-11-01). Boot log:
`pa1616d: stayed at 9600 after two PMTK251s; output timeout`. A timeout on
PMTK314 *at 9600*, the rate it is talking at, means nothing arrives at its
RX, and the ~5 sentences a second it keeps sending are its defaults (GGA,
GSA, GSV, RMC, VTG), not RMC+GGA. On that line the driver dumps the IO
matrix: GPIO3 is an output carrying signal 13 (`UART1_TXD_PAD_OUT`), so the
firmware is driving the right pin. Look at the board: U16 pin 6 (GPIO3)
soldered, the trace to U4-10, U4-10's own joint. To see the bytes, put the
FTDI's RXD (yellow) on U4-10 and GND on GND, open it at 9600 and reset the
P4: `$PMTK251,57600*2C` should arrive twice. If it arrives there and the
module still ignores it, suspect the level: the P4 drives 3.3 V into a
2.8 V module.

JP1 is an optional external antenna. The PA1616D switches to it on its
own when it draws current; nothing in firmware selects it.

The lap timer needs a fix before it will do anything at all. Do not judge
`core/delta` here: its thresholds are guesses that have never seen a real
trace, and tuning them is a track-day job.

---

## 8. `ctrl` — tach, wheels, shift lamps

Build-order step 7, and last on purpose: it is the latency-critical path
and you want the diag screens working before you touch it.

**First board (2026-10-28):** the tach wrap was fitted and gave **no
signal**; the R34/C19 filter below is the suspect, a hardware fix still to
make. No wheel sensors were fitted. The backlight lights, so the PCA9955B
answers on I2C1; the lamps are untested. On that ride the speedo read 0
with 9–13 satellites; a host test (`test_fusion`) shows fusion following
GPS with no wheels, so the cause is elsewhere and the logs on the card
are the next thing to read.

**Scope the tach before trusting the reading.** The pickup is an
inductive wrap on the spark plug lead, 5–10 V at most. It goes through
R34 (100 kΩ) with C19 (470 pF) to GND and D2 clamping to the rails — a
47 µs filter, and the spike from a wrap is a few µs or less. A 1 µs, 10 V
spike charges C19 to about 0.2 V, far below the input threshold, so **a
dead tach may be the filter, not the code** (`p4-board-pin-map.md` §6.3).
On the running bike, scope `TACH_SIG` at idle and at high rpm. If it does
not cross 2.5 V, fit C19 at 47 pF or leave it off; if that is still not
enough, drop R34 to 10–22 kΩ. Both are 0805.

With an edge arriving, the ringing after each spark still gives several
edges. The firmware has a refractory window after each accepted edge —
**`bsp::kTachRefractoryUs` is 1500 µs and says "tune on the bench" next to
it.** 1500 µs caps the reading at 40 000 rpm on a one-spark-per-revolution
pickup, so it will not get in your way; it is the ringing it has to
swallow, and only a real coil says how long that lasts.

Check rpm against a known idle before believing anything. Reading exactly
double or triple is the ringing; reading wildly high is the window being
too short; reading nothing is the filter above.

**Wheel speeds** have their own 200 µs window against contact chatter,
also unconfirmed — spin a wheel on a stand before trusting mph
(`engine_sense.cpp:89`). CN2 is the rear (`WHEEL_HALL_R`, GPIO23) and CN3
the front (GPIO22). Pin 1 of each is **VIN, not 3.3 V**: the sensor gets
12 V, and its output is pulled up to 3.3 V on the board. An open-collector
sensor is what this expects.

The sensors are Allegro A1120LUA-T unipolar Hall switches, not latches.
Each needs magnets, one per pulse, every one with its **south pole facing
the sensor's branded face**: a magnet fitted north-out gives no pulse, and
bare steel (sprocket bolts, rotor holes) gives none either, as the A1120 is
not back-biased. The count goes up as each magnet leaves (the rising edge),
and CONFIG → Wheels → Front/Rear pulses must equal the magnets fitted. The
A1120's supply is rated to about 28 V absolute (check the datasheet) and VIN
reaches ~29 V while the input SMBJ18A clamps; if that margin matters, put
~100 Ω in series with pin 1 and a 24 V zener at the sensor end of the
harness.

**The shift lamps are eight outputs of the PCA9955B** (OUT0–7) on I2C1,
anodes on VCC, 30 mA full scale set by REXT (R23). There is no master FET
any more. If the lamps stay dark, check DIAG → Bus for I2C1 first — the
same part runs the backlight, so dark lamps with a lit screen is a channel
or a lamp, and dark lamps with a dark screen is the part. Remember the
duty is capped on USB.

**The budget is 3 ms from a tach edge to a lamp.** That includes the I2C1
write, about 100 µs at 1 MHz. If you add anything to core 0 above
priority 18, you have broken it.

### 8.1 Power-fail

`power` (core 1, priority 22, 1 kHz) is the only thing that reacts to the
bike being switched off, and it has never run. With USB connected as well
as the bike supply, so the board survives the cut:

1. Run on the bike supply for a few seconds, then switch the ignition off.
   The serial console should print `power: VIN x.x V: supply cut. dark in N
   us, totals saved in M us`. The backlight and the lamps go dark.
2. **Read M.** The hold-up is about 9 ms at full load and the trip is at
   9 V, so what is really available after the trip is about 3 ms with
   everything on and 6 ms with the loads shed. If "dark" is more than a
   millisecond or "saved" more than ~3 ms, the NVS write cannot be trusted
   to finish on a real cut, and the choice is more capacitance (the 470 µF
   option in the pin map) or giving up on the odometer write.
3. With USB still holding the board up you should then see `supply back,
   resuming` about a second later, the backlight returns, and logging
   resumes. If it does not, `power_fail` is stuck.
4. Crank the engine with a weak battery. VIN dipping toward 9 V must not
   trip; if it does, lower `trip_v` in `core::power::FailDetector::Config`
   (and expect less time left).
5. Cut the supply with a scope on VIN and the 3V3 rail: that is the real
   test of how long the board has.

Not done, and not possible on this board: flushing the log file.

---

## 9. Storage, then the radio

Steps 8 and 9. The SD card is on SPI2 in SPI mode, not SDMMC, sharing the
bus with the IMU. Card detect is GPIO1, LOW with a card in.

**First board:** on 12 V the card mounts and the logger writes ~32 kB/s
with no errors. On USB alone it did not mount, which was the supply
(`p4-board-pin-map.md` §6.2). With a card in, the IMU and the SD host
deadlocked on SPI2 and the task watchdog rebooted the board; every IMU
transaction is now polling, and three 30 s boots with a card since had no
watchdog.

USB mass storage cannot be a partial implementation: the device must
unmount the card before a host touches it, or the FAT volume gets two
writers and is corrupted.

Do BLE **last**, and after the real-time path is known good — so that if
the shift-light timing changes, the radio is the only suspect. The P4 has
no radio of its own: BLE runs on the module's ESP32-C6 over its internal
SDIO via ESP-Hosted, as `demo_p4/` does on the demo board
(`bringup-p4-demo.md`). The C6 is a MINI-1U with an IPEX connector, so it
**needs an external antenna** fitted before range means anything.

The C6 has its own reset (GPIO54, U16-74) and boot (U16-18, R1) lines.
Its UART is on test pads **H1 = U0RXD** (U16-16, into the C6) and
**H3 = U0TXD** (U16-17, out of it); its boot pin has no pad, so forcing it
into download mode means clipping onto R1.

**Found on the first board (2026-10-28): the C6 does not speak ESP-Hosted.**
`sdmmc_card_init` succeeds and the very next CCCR read times out
(`sdio_card_fn_init failed`, `0x107`), the same at 40 MHz 4-bit and 5 MHz
1-bit, with the demo's identical ESP-Hosted settings. So it is the
module's C6, not the link, and the SDIO push from `slave_fw` cannot reach
it. **Its flash was blank** (2026-10-31: the 4 MB read back held 24 bytes
that were not 0xFF, no bootloader, no partition table): on its UART it
printed `invalid header: 0xffffffff` and watchdog-reset (`TG0_WDT_HPSYS`,
`LP_WDT_SYS`) several times a second. Expect the same on the next module.
Flash the C6 over its own UART once:

With an FTDI TTL-232R-3V3: **black** GND, **yellow** its RXD, **orange**
its TXD. **Red is 5 V: leave it unconnected.** It ends up as
`/dev/ttyUSB0`. Source ESP-IDF (`export.sh`) and run from the repo root.
`tools/c6_uart.py` does steps 1, 4, 6 and 7 (`listen`, `wait`, `backup`,
`flash`); `flash` refuses to run until a 4 MB backup exists, and
`inspect` reads the backup offline: bootloader, partition table, each app's
project, version (ESP-Hosted's own, for a `network_adapter`) and IDF, and
whether it was built for SDIO or SPI.

1. **Listen.** Board off. Black to GND, **yellow to H3** (the C6's TX).
   Open `idf.py -C demo_p4/c6_cp -p /dev/ttyUSB0 monitor --no-reset`, then
   power the board. Do not open the P4's `/dev/ttyACM0` meanwhile: that
   resets the P4, which resets the C6. The text is the first evidence of
   what Guition put on it; keep it.
2. **Wire for flashing.** **Orange to H1** (the C6's RX).
3. **Find R1's IO9 end.** Board off, meter on continuity: the end of R1
   that beeps to a 3.3 V point (VCC) is the wrong one; the other is IO9.
   Ground the IO9 end **through a 1 kΩ resistor**, not a bare wire: it
   still reads low (0.3 V against the 10 kΩ pull-up), and the wrong end
   would only pass 3.3 mA instead of shorting VCC.
4. **Download mode.** The C6's net names: **C6_BOOT is IO9** (on R1),
   **C6_RST is EN** (on R22, 10 kΩ pull-up and 1 µF). IO8 has no net; the
   module holds it high. With the 1 kΩ on C6_BOOT (it reads ~0.3 V),
   **tap C6_RST's signal end of R22 to GND** for half a second. The C6's
   own watchdog resets do not re-read the straps, so a blank C6 cycling
   through them stays at `boot:0xc (SPI_FAST_FLASH_BOOT)` however long you
   wait; only EN (or power-on) brings `rst:0x1 (POWERON),boot:0x4
   (DOWNLOAD(USB/UART0/SDIO_FEI_FEO))` and `waiting for download`. Only tap:
   the P4 drives GPIO54 too. Check with
   `python -m esptool --chip esp32c6 -p /dev/ttyUSB0 --before no-reset --after no-reset chip-id`.
   `boot:` on the C6's UART is the strap reading: 0xc is both high,
   0x4 is IO9 low.
5. **Park the P4.** On the first board the P4 kept resetting the C6
   through GPIO54, a pair of power-on resets every ~14 s, and each one
   killed a long esptool read part way (`No more data to read from the
   serial port`). Put the P4 in its own ROM bootloader, where GPIO54 is
   left alone:
   `python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before default-reset --after no-reset chip-id`.
   Listen to the C6's UART for 20 s: no new `ESP-ROM` banner means
   nothing is resetting it.
6. **Back up what is there**, 4 MB. At 115200 it took 6.5 minutes;
   460800 failed once, before the P4 was parked, so which of the two was
   to blame is not known:
   `python -m esptool --chip esp32c6 -p /dev/ttyUSB0 -b 115200 --before no-reset --after no-reset read-flash 0 0x400000 c6_factory_backup.bin`
   (`tools/c6_uart.py backup` does the same at 460800).
   `tools/c6_uart.py inspect` says whether it holds anything.
7. **Flash ESP-Hosted** (`demo_p4/c6_cp`, 2.12.8, Wi-Fi and BLE; build it
   first with `idf.py -C demo_p4/c6_cp build`), about 70 s at 115200:
   `cd demo_p4/c6_cp/build && python -m esptool --chip esp32c6 -p /dev/ttyUSB0 -b 115200 --before no-reset --after no-reset write-flash "@flash_args"`
8. **Run it.** Unclip the 1 kΩ from R1, unplug and replug the board (the
   P4 is parked), and read the P4's console. Expect `transport: Identified
   slave [esp32c6]` and the C6 reporting 2.12.8, then `ble: advertising`.
   The yellow wire can stay on to watch the C6's side at the same time.
   First board, 2026-10-31: all of that, SDIO at 40 MHz 4-bit, and the
   P4 booted once with no loop.
9. **Then `idf.py -p /dev/ttyACM0 c6-flash`**, so `slave_fw` holds the same
   image and later C6 updates go over SDIO, as on the demo. The boot log
   then says `c6_fw: C6 runs 2.12.8 #…, slave_fw has 2.12.8 #…: current`.

The `ble` service is built in (`services/src/ble_service.cpp`) and starts
last at boot. On the first board (2026-10-31) the Android app bonded and its
Feed lines arrived (`phone connected, waiting for encryption`, then
`bonded phone back`). Two things found on the way:

- **The bonded name is the short one.** The scan response carries
  `CRF250F` (the full `CRF250F-Dash` no longer fits beside the ANCS
  solicitation), so a phone stores the bond as `CRF250F`. The app looked
  for `CRF250F-Dash` among bonded devices, missed, and fell back to a scan
  that never finished. It now matches either (`~/phone`, `DashLink.kt`).
- **A P4 that reboots every ~14 s is a C6 that does not answer.**
  `transport: Not able to connect with ESP-Hosted slave device`, then
  `H_SDIO_DRV: Host is resetting itself`: ESP-Hosted restarts the whole P4,
  so every reading on the console jumps as the filters start over. On
  2026-10-31 it was a short from wires left dangling on the C6's pads.
  **Fixed in 1.61:** that was `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE`,
  now off, and `ble` listens for `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` (on
  the default event loop, which it now makes: ESP-Hosted posts there but
  never made it). Tested with the C6 held in reset from the console
  (`c6 hold` / `c6 release`, `main/serial_buttons.cpp`): `ble: radio lost`
  220 ms later, no reboot, the IMU stream unbroken, NimBLE's 2 s retries
  kept off the console, DIAG's BLE a fault. Bluetooth comes back only at
  the next power-on: released, the C6 boots but ESP-Hosted does not
  re-initialise the SDIO card. The read loop's feared tight loop did not
  happen; with the C6 in reset it waits on the interrupt line.
  **A C6 dead from boot is untested.** The 2026-10-31 loop restarted from
  the read path this setting now covers, so it should stop too, but
  `c6 hold` cannot show it: ESP-Hosted's own reset pulse at boot lets the
  C6 go.
- **The phone logged `Waiting for scan response` forever**, and the laptop
  hears the dash at −90 dBm from beside it. That is a radio with no
  antenna: step 2 below.

1. **Build the C6's firmware first**:
   `idf.py -C demo_p4/c6_cp set-target esp32c6 build`. `idf.py flash` writes
   it into `slave_fw`, and on boot the dash pushes it to the C6 over SDIO
   if the C6 reports anything older than 2.12. That boot restarts once
   after the push, and only once.
2. **Fit the antenna** on the C6's IPEX before judging range.
3. On the console expect `ble: C6 firmware 2.12.x`, then
   `ble: advertising as "CRF250F-Dash"`. DIAG → Radio says **BLE up**.
   **BLE failed to start** is the SDIO link or the C6's controller; the
   log line before it says which.
4. **Poke it from nRF Connect** before trusting the phone app. Connect,
   and accept the pairing request: Feed needs a bonded, encrypted link
   (architecture §11.3), and a write before that is refused. DIAG → Radio
   should say Phone **bonded**, Bonds **1**. Find service `6e5f0001-…`,
   write the text `T hello` to `6e5f0002-…`. The title appears on SPEED
   and MEDIA, and DIAG → Radio's last tag reads `T`. Disconnect: the title
   stays about ten seconds, then clears.
5. **Check the gate.** With nRF Connect bonded, connect from a second
   phone: it should be dropped (`ble: unknown phone outside pairing mode,
   refused`). Open CONFIG → Phone → Pairing mode, connect it again: it
   bonds, and Bonds reads 2. CONFIG → Forget phone sets Bonds to 0; both
   phones must then forget the dash in their own settings before pairing
   again.
6. **NimBLE runs on FreeRTOS timers**, not esp_timer
   (`CONFIG_BT_NIMBLE_USE_ESP_TIMER=n`, so its timeouts are not above
   `ctrl` on core 0). No board has run NimBLE this way. Try it on the demo
   first: bond, disconnect, walk out of range and back, reconnect without
   pairing again, and let the 30 s encrypt deadline expire on a phone that
   connects and never pairs. Any stall here is the FreeRTOS timer task
   (priority 1) not getting time.
7. **Re-measure tach to lamp with a phone connected and streaming**, on
   the same scope setup as §8. The budget is still 3 ms. ESP-Hosted
   creates its SDIO tasks at priority 23 with no core pinning, and `ble`
   lowers them to 8 after start-up. The log says so:
   `ble: esp_hosted tasks: N found, …`. After the link is up N should be
   at least 6. If it reads 0, a newer esp_hosted renamed its tasks and
   they are still at 23.

8. **Read the detail log after every early ride.** With CONFIG → Log →
   Detail log on (the default), each power-on writes `detail/<stem>_imu,
   _tach, _det, _evt, _diag.csv` beside `logs/<stem>.csv`. Pull the card
   and run `python3 tools/ridelog/ridelog.py /path/to/card` (`--session N`
   for an older one, `--plots DIR` with matplotlib). It reports, one
   assumption at a time: drops and card stalls, the reset reason
   (brownout is the wiring), tach ringing against the refractory window
   and missed or extra sparks, edge-to-lamp latency against 3 ms, each
   wheel's circumference error against GPS, fusion's gate use, IMU gaps,
   clipping, rest bias and mount, the vibration spectrum, the steering
   estimate, slide-gate rates, rpm/mph clusters against the gear table,
   and every health change. `_diag`'s DropXxx columns staying at 0 is the
   first thing to check: the rings hold about ten seconds of card stall.

### 9.1 Firmware slots: stable and dev

Do this before trusting any of it with a ride. Each step is something only
the real bootloader can prove. The UI and the decisions are host-tested;
the flash calls are not.

1. **USB flash**, and confirm `ota: ... stable`. DIAG → Update shows the
   version, `ota_0`, and Other slot `empty`.
2. **Install from SD.** Build a second image with a visible change (tag it:
   the version string is `git describe`), copy `build/dash.bin` to the
   card as `update/dash.bin`, then Install from SD, parked. Expect a
   percentage, then "installed". Other slot shows the new version.
3. **Refusals.** Spin the rear wheel (or set rpm) and try again: "failed:
   park first". Put `network_adapter.bin` there instead: "bad image: wrong
   chip".
4. **Boot dev once.** The dash restarts into dev: red DEV tag, Running says
   `dev`. Power-cycle it: it must come back stable with no input. Then a
   panic in dev (or a watchdog) must do the same: type `panic` on the USB
   console (`main/serial_buttons.cpp`; it calls abort()). First board,
   2026-11-01: both came back stable. Erase the test's core dump after
   (`esptool erase-region 0x23000 0x1d000`) or DIAG flags a crash.
5. **No boot pause.** Power on with a dev image installed: the dash goes
   straight to stable, with nothing on screen asking for input.
6. **USB to dev.** `python3 tools/flash_dev.py` writes the other slot and
   the dash still boots stable afterwards.
7. **Promote.** Boot dev once, then DIAG → Update → Promote to stable:
   Running says `stable` at once, with no restart, and the DEV tag goes.
   Power-cycle and it stays. From stable, Promote must say "run dev first";
   in dev and moving, "park first".
8. **Pull the card mid-install**: "read failed", the other slot reads
   empty or invalid, and stable is unaffected.
9. **Tach to lamp** with an install refused while moving: unchanged. An
   install while parked is allowed to make `ctrl` late (DIAG → System's
   late count); that is why it is parked only.

---

## 10. Every BRINGUP marker, in one place

| Where | What |
|---|---|
| `icm42688p.cpp:202` | IMU axis mapping, checked on the bench only; the riding check (§6 step 7) is to do |
| `icm42688p.cpp:137` | IMU data-ready interrupt configuration, from the register map only |
| `engine_sense.cpp:133` | Wheel-Hall debounce window, 200 µs, unconfirmed |
| `bsp/pins.hpp:99` | `kTachRefractoryUs` 1500 µs — "tune on the bench" |
| `ili9488.cpp:281` | A strip's head relies on the panel continuing a 0x2C write with 0x3C: wrong colours at a strip's left edge mean it does not |
| `drivers.hpp:10` | The general warning: what has run is in the status table at the top |
| `ble_service.cpp` `kHostedPrio` | ESP-Hosted's tasks lowered from 23 to 8. Check SDIO keeps up and `ctrl` is unaffected |
| `ota_service.cpp` | Rollback, install and promote: never run against the real bootloader (§9.1) |
| `icm42688p.cpp:221` `chip_ts` | Whether the FIFO carries the part's timestamp: read `Hdr` (§6) |
| `bsp/pins.hpp` `kGpsRxFullBytes` | UART1 hands on bytes after 8 / 2 idle characters: checksums and `ctrl` overruns (§7) |
| `services/watchdog.cpp` | Task-watchdog panic: trigger it once, time the reboot (§2) |
| `sdkconfig.defaults` `BT_NIMBLE_USE_ESP_TIMER=n` | NimBLE on FreeRTOS timers: bond, reconnect, encrypt deadline (§9) |

And the hardware values to settle on the bench, from
`p4-board-pin-map.md` §6: C19 and R34 (tach), R6 (neutral pull-up), and
whether SW2 holds up at the board's input current.

---

## 11. A note on `sdkconfig`

`sdkconfig.defaults` **only seeds `sdkconfig` when it is first created.**
Adding a line to the defaults does nothing to a tree that already has a
config. This has bitten this project twice: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`
silently did nothing for a while, and both BLE core pinnings sat at 0
against a documented requirement of 1.

Both times the build was clean and the symptom was invisible. If you
change something in `sdkconfig.defaults`, either delete `sdkconfig` and
let it regenerate, or set it in `sdkconfig` too — and check it read back.
The chip-revision setting in §2 is exactly this kind of change.

Beware also that some Kconfig symbols are **derived**:
`CONFIG_BT_CTRL_PINNED_TO_CORE` is an int computed from the
`..._TO_CORE_0` / `..._TO_CORE_1` choice, so editing the int by hand looks
like it worked, greps back correct, and is recomputed away on the next
build. Set the choice.

---

## 12. Testing from the laptop, hands off

What 2026-10-31/11-01 used to test the first board with nobody at it. All of
it is the USB console (`main/serial_buttons.cpp`) plus esptool.

**Opening the port without resetting the board.** pyserial lowers DTR before
RTS, and DTR low with RTS high is the USB-Serial-JTAG's reset. Open with both
`True`, then set `rts = False` before `dtr = False` (`tools/pod_keys.py`).
The reverse order (DTR low, then RTS low) is how to reset it on purpose.

**Console commands.** `btn A`..`btn E`, `long A`..`long E` (the pod, after
debounce); `c6 hold` / `c6 release` (the C6 in reset: what a dead radio
does); `panic` (abort(): the core dump, the reboot, dev falling back to
stable). Send a bare newline first: half a line from an earlier session is
dropped after 500 ms, but only if nothing follows it sooner.

**Menu routes** (from anywhere; `long A` is CRUISE screen 0; presses 350 ms
apart; the sim shows the same screens, `simshot`-style, if one stops working):

| What | Presses |
|---|---|
| Wi-Fi on/off | `long A`, A×4, D×13, E, E — then E again toggles |
| Eject card | `long A`, A×4, D×12, E, E |
| DIAG → Update | `long A`, A×3, D×7, E |
| Boot dev once | then D, E |

**Before fuzzing the menus, back up NVS** (settings, the phone's bond, trip,
waypoints): `esptool read-flash 0x11000 0x10000 nvs.bin` twice and compare;
afterwards `esptool write-flash 0x11000 nvs.bin` and read it back. Random
presses reach Forget phone, Calibrate level, the trip and gear resets.
Tracks need a fix, so indoors they are safe.

**The console's 10 s summary** (`keeper`) is what a soak watches: `heap`
(internal free; a leak is it falling), `imu` (most samples found waiting at
one wake; the FIFO holds ~128), `timing` (ctrl late ticks, render rate),
`gnss`, `power`, and `logger`'s bytes and slowest write.

**What it found on the first board:** 19 min and 2337 random presses, no
reset; Wi-Fi 20 on/off cycles with RAM back each time; Eject flushing ~20 kB
the logger used to drop; a lost C6 no longer rebooting the dash; dev boot,
reset and panic all falling back to stable; a 40 min soak on 1.61 with the heap flat at 306 kB free, 0 late ctrl ticks, render at 120 Hz, 76 MB to the card with 0 errors, the IMU at most 77 samples behind.
