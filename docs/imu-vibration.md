# IMU vibration plan — ICM-42688-P (U1)

**Status.** A plan, not built. Nothing here has run on the board (it
arrived on 2026-10-28; the IMU has run on the bench only). Every number marked *estimate* is arithmetic from assumptions, not
a measurement; Phase 0 exists to replace them with data.

## 1. The problem, precisely

The attitude filter, the jump detector and fusion stage 1 all consume a
**200 Hz** IMU stream (`drivers/src/icm42688p.cpp`: ODR 200 Hz, gyro and
accel in low-noise mode, everything else left at its reset value).

1. **Aliasing.** Nyquist is 100 Hz. At reset the gyro anti-alias filter is
   ~585 Hz and the UI filter ~100 Hz (datasheet Rev 1.2, §5.3 and the
   `GYRO_UI_FILT_BW` table), so nothing above 100 Hz is rejected before
   decimation. Energy near n × 200 Hz folds to DC; within ~10 Hz of it folds
   to a slow wobble inside the lean filter's pass band.
2. **Engine orders.** Single-cylinder four-stroke: crank-rate vibration is
   rpm/60 Hz, with harmonics. *Estimate:* 2nd order lands on 200 Hz at
   6,000 rpm, 3rd at 4,000 / 8,000, 4th at 3,000 / 6,000 / 9,000. Idle
   (~1,700 rpm, *estimate*) puts the 1st order near 28 Hz.
3. **Terrain** is real motion below ~90 Hz and passes the sensor correctly.
   It is not what this plan removes. Rock strikes are broadband and alias
   like engine energy does.
4. **Steering coupling.** The dash is on the handlebar, so the IMU turns
   with the steering. The mount offset removes a fixed tilt, not this. Not
   fixable by filtering; see §5.
5. **No data.** The datasheet gives no vibration-rectification figure for
   either sensor. The 25–29 kHz gyro mechanical frequency is far from
   anything a bike makes.

## 2. Strategy

Three layers, each cheap and independent, in this order of leverage:

1. **Sample fast, filter in the right place** (firmware): kill aliasing at
   its source. This is the one fix the pads cannot substitute for.
2. **Remove engine energy digitally** (software): a tracking notch driven
   by the tach rpm the firmware already has, plus a vibration gate so the
   estimators know when to distrust themselves.
3. **Isolate mechanically** (mount): attenuate what is above the pad
   resonance, tuned to *this* engine's idle, without adding a resonance
   inside the terrain band.

And a measurement layer under all three, **built first**.

## 3. Phase 0 — measure (build before the first rides)

- **`vibcap` capture.** A DIAG action that records raw IMU samples at
  ≥1 kHz plus rpm and speed to the SD card for N seconds (60 s default).
  `imu` publishes into a `TrackBuffer`; `logger` (core 1) writes the file.
  12 B/sample at 1 kHz is ~12 kB/s, well inside the card's write budget.
  Core 0 does no filesystem work (CLAUDE.md core split).
- **`tools/vib_spectrum.py`** (host): reads a capture, plots a spectrogram
  against rpm with the engine orders (0.5×, 1×, 2×, 3×, 4×) and the
  n × ODR alias lines overlaid, and prints the in-band RMS and 0–20 Hz
  energy. This is the instrument for every later decision.
- **Host tests** for the file format, like `core/log`.

Exit: a capture file from a bench run (sander or speaker on the bar) and one
from the bike at idle, readable by the script.

## 4. Firmware (`drivers/icm42688p`, `services/imu_service`)

All values below come from the datasheet tables and are **BRINGUP: never
seen a real part**; verify register writes by reading them back.

- **ODR 1 kHz** (`GYRO_ODR`/`ACCEL_ODR` = 6). Nyquist 500 Hz.
- **Anti-alias filters enabled, ~258 Hz**, both sensors: gyro
  `DELT 6 / DELTSQR 36 / BITSHIFT 10` (bank 1, 0x0C–0x0E), accel the same
  (bank 2, 0x03–0x05, `ACCEL_AAF_DELT` is bits 6:1). Needs a bank-select
  helper (`REG_BANK_SEL` 0x76), restored to bank 0 afterwards. AAF only
  works in low-noise mode, which `kPwrMgmt0 = 0x0F` already sets.
- **UI filter** left at its default (2nd order is available if the host
  filter needs help; decide from Phase 0 data).
- **FIFO watermark interrupt**, not data-ready per sample: 1,000 interrupts
  a second on core 0 is wasteful. Watermark at ~10 packets gives a 10 ms
  cadence. 16-byte packets, 2 kB FIFO = ~128 samples, so the read must
  never be slower than ~100 ms. `imu_service`'s timeout fallback stays.
- `kSamplePeriodMs` becomes 1 and gets a compile-time tie to the ODR
  constant, because today nothing notices if they drift apart.
- **Clip flags**: a sample at full scale is marked, so downstream code can
  ignore it instead of trusting a clipped landing.
- Keep ±16 g / ±2000 dps. Do not lower the range for resolution; the
  existing comment explains why.

## 5. Software (`components/core`, host-tested, no ESP headers)

New `core/imu_dsp/`:

1. **Decimator**: 1 kHz in, 200 Hz out, 4th-order Butterworth low-pass as
   cascaded biquads (corner ~50–60 Hz, tune from Phase 0) then ÷5. Fixed
   storage, no allocation after boot. The 200 Hz stream keeps feeding
   `attitude`, `jump` and `fusion` unchanged, so none of them is touched.
2. **Tracking notch** at crank order 1× and 2× (rpm/60, rpm/30), coefficients
   refreshed every ~20 ms from the tach slot. Needed because idle's 1st
   order (~28 Hz) is *below* the low-pass corner, so the low-pass cannot
   remove it. Bypassed when rpm is invalid or the tach is faulted.
3. **Peak path for jumps**: the landing peak is taken from the unfiltered
   1 kHz accel magnitude, not the 55 Hz-filtered one, or `landing_g` would
   under-read short impacts. Free-fall detection stays on the filtered
   stream.
4. **Vibration gate**: RMS of the high-passed accel, published as a health
   value. `attitude` widens its accelerometer gate (it currently fails
   almost constantly on rough ground, so low-speed lean drifts) and
   `fusion` lowers IMU trust while it is high.
5. **Steering coupling**: partly corrected in `core/attitude`: the
   steady-corner steering angle is estimated from yaw rate, speed and
   wheelbase, and the readings are turned back about the head tube. That
   covers the steady part. Countersteer, bar shake and a bike stopped with
   the bars turned have no source to correct from. Phase 3 compares
   bar-mounted lean against the stand/GPS reference. If it is bad, the
   fallback is a second IMU rigid to the frame, or moving the dash.

**Tests** (rule: every change to `core/` needs one): a tone at 190 Hz in is
>40 dB down out; DC and 5 Hz pass with <1 dB error; the notch removes a
swept tone and leaves a 5 Hz tone alone; clip flags propagate; nothing
allocates. `sim/` gets a vibration slider that injects engine orders, so
the effect is visible in the lean readout.

## 6. Physical mount

`mount/pod.py` (2026-10-28) is the isolated design, open, no housing: the
board on a tray floating on four silicone gimbal damping balls in front of an
H-frame on the bar clamp. Nothing limits its travel but the balls. `mount/mount.py` is the earlier rigid carrier. What
the isolation must do:

- **Revise the target frequency.** I said ~30 Hz earlier. That was wrong
  for this engine: idle's 1st order is ~28 Hz (*estimate*), so a 30 Hz mount
  would amplify idle. Isolation only begins above 1.4 × fn. **Target
  fn ≈ 15–20 Hz**, which attenuates from ~25–28 Hz upward.
- **Softer pads.** Total stiffness for 20 Hz on a ~70 g board and panel
  (*weigh it*) is about 1.1 kN/m, ~280 N/m per mount. The current 1.5 mm
  pad is far too stiff. Move to **~2.5 mm compressed per side** of soft
  closed-cell foam (`PAD_T` in `mount.py`), and check sag is ~1 mm.
- **Damping over softness.** Foam beats O-rings: more damping, less peak at
  resonance. Terrain chatter (20–80 Hz) sits at the resonance, so a
  lightly damped mount makes it worse.
- **Travel limits.** The screw shank in the 3.0 mm hole limits sideways
  motion (~0.5 mm each way); confirm it also limits sag so the board cannot
  hit the frame, and keep ≥3 mm air around the board edge.
- **Cables bypass isolation.** Slack loops on every connector, strain relief
  on the frame side, nothing pressing against the board.
- **IMU location is already good**: U1 at (16.9, 26.5) is ~10 mm from the
  lower-left hole, the stiffest part of the board.
- **Carrier stiffness**: arms and posts should have their first mode well
  above 150 Hz. Tap-test with `vibcap`; if low, shorten `SPACER` or widen
  the gussets.
- **Bar-end vs centre**: keep the clamp near the bar centre, not toward a
  grip, where bar flex is largest.

## 7. Verification

| Test | Setup | Pass |
|---|---|---|
| Alias floor | hold constant rpm at 4,000 / 6,000 / 8,000 | no tone above the stationary 0–20 Hz floor |
| Idle gyro | on the stand at idle, 60 s | yaw RMS within 2× the engine-off figure |
| A/B/C | rigid · pads · pads + notch, same loop | 0–20 Hz energy falls A→C |
| Lean | stand tilt vs tape reference, then a ride | lean within a few degrees, no drift on a long straight |
| Jumps | whoops, jumps, washboard | no false take-offs, landing peak plausible |
| Pad tune | tap test and idle sweep | no resonant peak near idle or in 20–80 Hz |

Pass criteria are first guesses; tighten them on the first real data.

## 8. Order of work

0. `vibcap` + `tools/vib_spectrum.py` + format tests. Update `mount.py`
   `PAD_T` and the BOM line.
1. `core/imu_dsp` with tests, then the sim slider.
2. Driver: ODR, AAF, watermark FIFO, clip flags (reads back its registers
   at init and reports mismatches to `health`).
3. At bring-up: baseline capture rigid, with the old 200 Hz config.
4. Enable the new path; capture again; compare.
5. Pads on; tune thickness from the capture; ride tests; tune the corner
   and notch depth.

## 9. Open questions

- Real idle rpm and redline (the 1×/2× arithmetic depends on them).
- Board + panel mass.
- Whether the P4's SPI2 sharing with the SD card allows a 10 ms FIFO cadence
  during card writes (`logger` blocks on the card lock).
- How much steering coupling the bar mount really adds.
