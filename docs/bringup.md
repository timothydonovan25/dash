# Bring-up

§17 of the architecture document is a *build order* — what to write, in
what sequence. This is the other half: when the board is on the bench,
what to do, what you should see, and what each failure means.

This is for the **P4 board**: the Guition JC-ESP32P4-M3-C6 module (U16),
the panel on an 8-bit 8080 bus, the shift lamps and backlight on a
PCA9955B (U2). `p4-board-pin-map.md` is the pin truth and its §6 is the
pre-order audit; read §6 before starting here.

Nothing in this repository has run on this board. Every driver is written
against a datasheet and a netlist, which is not the same as correct. The
places that can only be settled here are marked `BRINGUP` in the source
and collected in §10 below.

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
you want before the first `idf.py flash`.

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
`sdkconfig.defaults` has `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` because
the demo board was v1.3. If yours is v3 or later, that setting has to
change (§11 explains why editing the defaults alone is not enough).

**What you should see.** `dash v2 — N config items`, then twelve tasks
starting (`ctrl`, `imu`, `gnss`, `logic`, `input` on core 0; `render`,
`logger`, `storage`, `keeper`, `ota`, `ble`, `wifi` on core 1), then `running`.
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
and pod, then the other tasks. The GPS (half a second of baud negotiation)
and the SD mount happen on `gnss` and `storage`, after the dash is already
drawing. A power-on also skips the bootloader's hash of the image
(`CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`); a software restart still
checks it. The estimate is about half a second from the switch to lamps
and screen, never measured: the log's millisecond stamps from the first
`I (` line to `main: running` are the number to write down here, and a
scope on 12 V against the backlight gives the rails as well.

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
| Image mirrored or upside down | `ili9488.cpp:94` — `swap_xy` plus **one** mirror. Marked BRINGUP; this is the one you are expected to fix here. `panel_test`'s corners screen shows which. |
| Torn, speckled, or wrong colours | Drop `bsp::kTftWrHz` (starts at 10 MHz) before suspecting anything else. Swapped red/blue is the byte order, not wiring. |
| Colours right, garbage in places | A data line: D0–D7 are panel pins 17–24, which land on connector pads 24–17, from GPIO33 down to GPIO26. |

The panel runs **RGB565, two bytes per pixel**, on this bus: 30.7 ms per
full frame at 10 MHz, about 15 MHz in spec. `render` runs at 60 Hz and sends only
changed widgets: on SPEED, the tach bar and speed tile are about 4.7 ms
of a 16.7 ms period. A full repaint takes two periods. If you find yourself
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

**100 kHz, not 400.** Already set in `bsp/buses.cpp` with the reason in a
comment. 4.7 k pull-ups (R35/R36) over half a metre give ~470 ns of rise
time; fast mode allows 300 ns. At 400 kHz this works on the bench and
fails in the rain, which is the worst possible failure mode.

**There is no interrupt line on CN1.** The pod is polled at 100 Hz. Do not
go looking for an INT pin. CN1 is 1 SDA, 2 SCL, 3 3.3 V (through R31 and
the F2 polyfuse), 4 GND — check the pod's harness against that.

| Symptom | Meaning |
|---|---|
| All five buttons read pressed | IPOL inversion, or the pod came back from a brown-out with default registers. `refresh_config()` exists for exactly this. |
| Nothing responds, ever | Bus wedged, or SDA/SCL swapped in the harness. Watch **I2C recover** climb on DIAG → Bus. |
| Buttons work, then stop after a jump | Brown-out. Expected; recovery should handle it. If **still stuck** climbs too, it is the cable. |
| Pod dead, F2 hot | A short in the cable; the polyfuse is doing its job. |

**Neutral** is separate: GPIO12 (U16-19), opto-isolated by U6 and pulled
up by R64, and it reads **LOW when in neutral**. A neutral light that reads
inverted is this, not a bug. Before connecting it to the bike, measure
the stock neutral wire: the dash pulls it to about 12 V through R6 when
in gear (`p4-board-pin-map.md` §6.2).

---

## 6. The IMU

Build-order step 5. **The axis mapping has never been checked against a
real part** (`icm42688p.cpp:180`, BRINGUP). Nor has the data-ready
interrupt setup (`icm42688p.cpp:132`): if it is wrong the service falls
back to polling, and DIAG → System counts how often.

Do this statically before trusting any filter output:

1. Bike upright and still. `g_vert` ≈ 1.0, `g_lat` ≈ 0, lean ≈ 0.
2. Lean it to the right. Lean angle should go **positive**, and the marker
   on the speed screen's gauge should move right.
3. Lean it left. Negative.
4. Brake hard by hand (rock it forward). `g_lon` sign.

Fix the mapping in the driver, not in `core/attitude` — the filter's
convention is documented and the rest of the code depends on it.

Only then tune the filter against a bench tilt. Lean comes from `v·ω`, not
from the accelerometer, so it will read nonsense while stationary and that
is correct.

The IMU shares SPI2 with the SD card. If IMU samples arrive in bursts
while logging, that is SD write-busy time; the FIFO absorbs it. If the IMU
faults intermittently at 24 MHz, drop `bsp::kSpiHzImu` toward 10 MHz —
the bus is about 90 mm long with no source termination.

---

## 7. GPS

Build-order step 6. Take it outside. **Every power-up is a cold start**:
VBACKUP (U4-4) is tied to VCC, so the module keeps nothing when the dash
is off. Expect minutes, not seconds.

The module boots at 9600 baud and the driver moves it to 57 600
(`kGpsBootBaud`, `kGpsRunBaud`). 10 Hz with more than RMC+GGA does not
fit in 9600, so a fix that arrives and then stalls is the baud switch.

DIAG → GPS shows sentence count and bad-checksum count. **Sentences
climbing with checksums climbing** is a baud or wiring problem.
**Neither climbing** is nothing arriving at all: U4-9 (module TX) goes to
GPIO2, U4-10 to GPIO3. The parser is host-tested against captured
sentences, so a parse bug is unlikely — the suspicion should fall on
UART1 first. The module's output is 2.8 V logic against the P4's ~2.5 V
input threshold: workable, but a marginal edge looks like checksum
errors.

JP1 is an optional external antenna. The PA1616D switches to it on its
own when it draws current; nothing in firmware selects it.

The lap timer needs a fix before it will do anything at all. Do not judge
`core/delta` here: its thresholds are guesses that have never seen a real
trace, and tuning them is a track-day job.

---

## 8. `ctrl` — tach, wheels, shift lamps

Build-order step 7, and last on purpose: it is the latency-critical path
and you want the diag screens working before you touch it.

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
Its UART is on test pads H1/H3; its boot pin has no pad, so forcing it
into download mode means clipping onto R1.

The `ble` service is built in (`services/src/ble_service.cpp`) and starts
last at boot. It has not run on this board. The same GATT code advertised
as "CRF250F-Dash" on `demo_p4/`, and nothing has connected to either yet.

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
6. **Re-measure tach to lamp with a phone connected and streaming**, on
   the same scope setup as §8. The budget is still 3 ms. ESP-Hosted
   creates its SDIO tasks at priority 23 with no core pinning, and `ble`
   lowers them to 8 after start-up. The log says so:
   `ble: esp_hosted tasks: N found, …`. After the link is up N should be
   at least 6. If it reads 0, a newer esp_hosted renamed its tasks and
   they are still at 23.

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
   panic in dev (or a watchdog) must do the same.
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
| `icm42688p.cpp:180` | IMU axis mapping, never checked against a real part |
| `icm42688p.cpp:132` | IMU data-ready interrupt configuration, from the register map only |
| `engine_sense.cpp:89` | Wheel-Hall debounce window, 200 µs, unconfirmed |
| `bsp/pins.hpp:81` | `kTachRefractoryUs` 1500 µs — "tune on the bench" |
| `bsp/pins.hpp:51` | `kTftWrHz` 10 MHz — raise to what `panel_test` stays clean at |
| `ili9488.cpp:94` | Panel rotation — `swap_xy` plus one mirror |
| `drivers.hpp:10` | The general warning that none of this has run |
| `ble_service.cpp` `kHostedPrio` | ESP-Hosted's tasks lowered from 23 to 8. Check SDIO keeps up and `ctrl` is unaffected |
| `ota_service.cpp` | Rollback, install and promote: never run against the real bootloader (§9.1) |

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
