# Bring-up

§17 of the architecture document is a *build order* — what to write, in
what sequence. This is the other half: when the board is on the bench,
what to do, what you should see, and what each failure means.

Nothing in this repository has run on hardware. Every driver is written
against a datasheet and a netlist, which is not the same as correct. The
places that can only be settled here are marked `BRINGUP` in the source
and collected in §9 below.

**The order matters more than the speed.** Each session below assumes the
one above it works. Skipping ahead means debugging two unknowns at once,
which is how a week goes.

---

## 0. Before you apply power

Three of these are irreversible if you get them wrong.

**Check FPC pins 1–8 against the panel's own datasheet.** This is the
highest-risk item on the whole board and it is a datasheet read, not a
rework — see `dash-v1-netlist-review.md` §1. Pins 1–8 are unconnected in
the netlist. If your panel expects one of them to be a supply or a mode
strap, powering it as-is can destroy it. Do this before anything else.

**Set the partition table before the first flash.** Changing it later
erases NVS, and NVS holds your odometer. `partitions.csv` is already in
the tree and `sdkconfig.defaults` points at it; just confirm it is what
you want before the first `idf.py flash`.

**Know your bench current budget.** D7 feeds the 5 V rail from VUSB, so
on the bench everything runs from the USB port: about **610 mA with all
eight shift lamps lit, against a 500 mA USB 2.0 limit**
(`dash-v1-netlist-review.md` §5). The firmware already clamps the lamp
PWM to `bsp::kShiftDutyUsbMax` until `tach.running()`, so this should not
bite — but if you see the board brown out when the lamps come on, that is
what it is. A browning-out expander takes the panel's reset line with it,
so it looks like a display fault.

**Check C22 and C23's voltage rating** (§6) and note that U12 and U13 are
in the BOM with zero connections (§2), so do not go looking for them in
firmware.

---

## 1. It boots at all

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py -p /dev/ttyACM0 flash monitor
```

**What you should see.** One log line naming the config item count, then
nine tasks starting. If the config table is malformed it says so here
rather than weeks later as a setting that will not save.

**If it reboots in a loop**, read the coredump — `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`
is already on. A panic before the first log line is almost always PSRAM
or the partition table, not your code.

**If it hangs with no output**, check the USB-serial-JTAG pins (IO19/IO20)
are not being driven by anything else.

At this point nothing is on the screen and that is expected.

---

## 2. The expander, then the panel

Build-order step 2. **Order matters here and it is not the obvious one.**

**TFT_RST is on the MCP23S17 (GPB6), not on a GPIO.** The SPI expander has
to be up before the panel can be reset. R3 pulls the line high, so a dead
expander degrades to "panel never resets" rather than bricking anything —
but it means a display fault and an expander fault look identical from the
front.

**What you should see.** The boot screen, then the SPEED screen drawing.

| Symptom | Where to look |
|---|---|
| Backlight on, white screen | Panel got power but no init. Suspect the expander, not the panel. |
| Backlight off entirely | LEDA / LEDK1-3 wiring, or the LEDC channel in `ili9488`. |
| Image mirrored or upside down | `ili9488.cpp:77` — `swap_xy` plus **one** mirror. Marked BRINGUP; this is the one you are expected to fix here. |
| Torn or speckled | Drop the SPI clock below 40 MHz before suspecting anything else. |

The panel is 480×320 landscape, RGB666, three bytes per pixel. It does not
accept RGB565 in SPI mode. If you find yourself writing panel command
sequences, stop — that is the `esp_lcd_ili9488` managed component's job.

---

## 3. DIAG → Sensors, and DIAG → Bus

**This is the week these screens were built for.** Get here early.

DIAG → Sensors is one row per device: `absent` / `init` / `ok` /
`degraded` / `FAULT`. Work down it. Anything `absent` is either genuinely
not fitted (ECU, BLE, TPMS on this revision) or never reported at all.

DIAG → Bus answers a different and more useful question: *is the device
dead, or is the bus dead?* "SPI2 imu FAULT" with "SPI2 mcp ok" is a part;
both faulted is the bus.

Two rows on that screen only matter once something has gone wrong:

- **I2C recover** — how many times the pod's bus was clocked free.
- **still stuck** — how many of those failed to release SDA. Non-zero
  means a wiring fault, not a glitch. A pod that browns out and recovers
  increments the first and not the second.

---

## 4. The handlebar pod

**100 kHz, not 400.** Already set in `bsp/buses.cpp` with the reason in a
comment. 4.7 k pull-ups over half a metre give ~470 ns of rise time; fast
mode allows 300 ns. At 400 kHz this works on the bench and fails in the
rain, which is the worst possible failure mode.

**There is no interrupt line on CN1.** The pod is polled at 100 Hz. Do not
go looking for an INT pin.

| Symptom | Meaning |
|---|---|
| All five buttons read pressed | IPOL inversion, or the pod came back from a brown-out with default registers. `refresh_config()` exists for exactly this. |
| Nothing responds, ever | Bus wedged. Watch **I2C recover** climb on DIAG → Bus. |
| Buttons work, then stop after a jump | Brown-out. Expected; recovery should handle it. If **still stuck** climbs too, it is the cable. |

Neutral is separate and is **LOW when in neutral** — opto-isolated, pulled
up on GPB5. A neutral light that reads inverted is this, not a bug.

---

## 5. The IMU

Build-order step 5. **The axis mapping has never been checked against a
real part** (`icm42688p.cpp:149`, BRINGUP).

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

---

## 6. GPS

Build-order step 6. Take it outside; a cold start is minutes, not seconds.

DIAG → GPS shows sentence count and bad-checksum count. **Sentences
climbing with checksums climbing** is a baud or wiring problem.
**Neither climbing** is nothing arriving at all. The parser is
host-tested against captured sentences, so a parse bug is unlikely — the
suspicion should fall on UART1 first.

The lap timer needs a fix before it will do anything at all. Do not judge
`core/delta` here: its thresholds are guesses that have never seen a real
trace, and tuning them is a track-day job.

---

## 7. `ctrl` — tach, wheels, shift lamps

Build-order step 7, and last on purpose: it is the latency-critical path
and you want the diag screens working before you touch it.

**The tach input has no filter cap.** An inductive pickup on an ignition
lead produces a damped ringing burst, not a clean pulse, so the GPIO sees
several edges per spark (`dash-v1-netlist-review.md` §7). A 470 pF from
TACH_SIG to GND is one part and fixes it properly. The firmware also has a
refractory window after each accepted edge — **`bsp::kTachRefractoryUs` is
1500 µs and says "tune on the bench" next to it.** 1500 µs caps the reading
at 40000 rpm on a one-spark-per-revolution pickup, so it will not get in
your way; it is the ringing it has to swallow, and only a real coil says
how long that lasts.

Check rpm against a known idle before believing anything. Reading exactly
double or triple is the ringing; reading wildly high is the window being
too short. The wheel speeds have their own 200 µs window against contact
chatter, also unconfirmed — spin a wheel on a stand before trusting mph
(`engine_sense.cpp:90`).

**The shift lamps need two things to be true**: the mask on the expander,
and the master FET on IO16 open. Both are behind `hal::IShiftLamps`
because they are useless apart. If the lamps stay dark with a correct
mask, the gate is the suspect — and remember the duty is deliberately
clamped until the engine turns.

**The budget is 3 ms from a tach edge to a lamp.** If you add anything to
core 0 above priority 18, you have broken it.

---

## 8. Storage, then the radio

Steps 8 and 9. USB mass storage cannot be a partial implementation: the
device must unmount the card before a host touches it, or the FAT volume
gets two writers and is corrupted.

Do BLE **last**, and after the real-time path is known good — so that if
the shift-light timing changes, the radio is the only suspect. The
controller and host are pinned to core 1 and `main.cpp` fails the build if
that ever drifts.

---

## 9. Every BRINGUP marker, in one place

| Where | What |
|---|---|
| `icm42688p.cpp:149` | IMU axis mapping, never checked against a real part |
| `engine_sense.cpp:90` | Wheel-Hall debounce window, 200 µs, unconfirmed |
| `bsp/pins.hpp:62` | `kTachRefractoryUs` 1500 µs — "tune on the bench" |
| `ili9488.cpp:77` | Panel rotation — `swap_xy` plus one mirror |
| `keeper_service.cpp:107` | No VIN sense this revision; `Elec` stays zero on purpose |
| `drivers.hpp:10` | The general warning that none of this has run |

---

## 10. A note on `sdkconfig`

`sdkconfig.defaults` **only seeds `sdkconfig` when it is first created.**
Adding a line to the defaults does nothing to a tree that already has a
config. This has bitten this project twice: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`
silently did nothing for a while, and both BLE core pinnings sat at 0
against a documented requirement of 1.

Both times the build was clean and the symptom was invisible. If you
change something in `sdkconfig.defaults`, either delete `sdkconfig` and
let it regenerate, or set it in `sdkconfig` too — and check it read back.

Beware also that some Kconfig symbols are **derived**:
`CONFIG_BT_CTRL_PINNED_TO_CORE` is an int computed from the
`..._TO_CORE_0` / `..._TO_CORE_1` choice, so editing the int by hand looks
like it worked, greps back correct, and is recomputed away on the next
build. Set the choice.
