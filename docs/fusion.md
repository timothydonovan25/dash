# Sensor fusion

The bike has three motion sensors, and each one is good where another fails:

| Sensor | Good at | Bad at |
|---|---|---|
| GPS (10 Hz) | absolute position and speed, no drift | slow, late, glitches, no heading below ~3 mph |
| IMU (200 Hz) | sees a change at once | drifts in seconds when integrated, vibration |
| Wheel speed | exact, every pulse | rear spins under power, front locks under braking, no direction |

Fusion blends them in `core/fusion/`: pure logic, host-tested, run by
`logic` at 50 Hz, published in the `fused` slot. The raw readings stay in
`motion` and `gnss`; the safety gates (parked-only OTA, the Wi-Fi moving
shut-off) keep using what was measured.

## Stages

1. **Road speed. Built.** `core::fusion::SpeedFilter`, below.
2. **Heading**, from the yaw gyro (corrected for lean) and GPS course: a
   heading that holds at walking pace and through a dropout, for NAV and
   the maps. Not written.
3. **Position**: dead reckoning between fixes on the stage 2 heading and
   the stage 1 speed, for 50 Hz finish-line crossings and the lap delta.
   Not written.

## Stage 1: speed

A two-state Kalman filter: speed in mph, and the forward accelerometer's
bias in g.

- **Predict** each tick from `attitude.g_lon`, minus the learned bias.
- **Correct** from GPS Doppler (once per fix, noise scaled by HDOP), then
  each wheel.
- **Gate**: a correction more than `gate_sigma` from the prediction is
  rejected. That keeps out a GPS glitch.
- **GPS referees the wheels.** For a second after an accepted fix, a wheel
  more than 4 mph (or a quarter of the GPS speed) from it is not believed.
  That catches a dead sensor reading 0 at speed, which the gate alone would
  follow, because it would drag the estimate down with it. **Only a fix
  with HDOP 2.5 or better referees** (1.61): parked under a bridge, GPS
  drifting to 6 mph at HDOP 4 was accepted (its noise scales with HDOP, so
  the gate is wide), refereed, threw out two good wheels at 0, and the
  filter showed 6.4 mph standing still. A poor fix is still used, weakly.
- **Wheel split**: front and rear more than 2 mph apart means one is
  spinning or locked; only the one nearer the referee (or the filter) is
  used. That catches a spin that builds gently enough to slip through the
  gate.
- A source rejected for 1.5 s straight is **suspect**. If every offered
  source is suspect, the filter is the one that is wrong, and it restarts
  on GPS.
- **GPS outvoted by wheels reading 0** (1.60). Wheels that read 0 under a
  moving bike (unfitted, unplugged) are agreed with while the filter sits
  at 0, so they are never suspect and the rule above never fires. GPS
  suspect, above 10 mph with HDOP 2.5 or better (more than any standstill
  drift), with every offered wheel under 1 mph: the filter
  restarts on GPS, and the referee then keeps the wheels out. Without it the
  first rides (s156, s157, 2026-10-29) showed 0 at up to 50 mph.
- **A rejected fix with no wheel accepted widens the filter** (1.60) until
  that fix would just have passed. Nothing else is checking the
  accelerometer then, so a coasting filter that has run away (1 Hz fixes, a
  bad g_lon) is pulled back by the next fix instead of gating it out.
- With no correction for 2 s it stops claiming to be **valid**.

Replayed over s156/s157 with both: the fused speed is within 3 mph of GPS
at 86 % and 89 % of moving fixes (it read 0 throughout on the bike). What
is left is s156's attitude: the board was not held rigidly on those rides
(15 g, 1100 dps, gravity 60° apart between the two parked spells), so
`g_lon` was nonsense and the filter coasts away between fixes for up to
1.5 s. A rigid mount, Calibrate level, and 10 Hz GPS fix that.

On screen: DIAG → GPS shows "Fused mph" with its 1σ and which sources are
in use (`F R G`, lower case = suspect). CONFIG → Display → Speedo picks
what the big number shows: Wheels (the default) or Fused. **Leave it on
Wheels until the filter has been tuned on real rides.**

"Wheels" is not blind to the filter any more (1.36). `ctrl` picks
`Motion::best_mph` -- the speedo, the odometer, the gear and its calibration,
the blackbox -- with `core::fusion::shown_speed_mph`: the front wheel, unless
the filter rejected it on its last pass or it is more than 4 mph (or a
quarter) from the fused speed, and then the fused speed; with no valid fused
speed, the rear, then GPS, as before. On a clean ride that is the front
wheel every tick. What it changes is a wheelie (the front spinning down in
the air read as the bike slowing, by half in 3 s), a locked front (0 mph)
and a sensor that has just died (0 mph for the 5 s before WheelCheck calls
it). `replay --scenario all` prints it as "shown". The cost of an untuned
filter is that it can overrule a front that was right; GPS still corrects
the fused speed, so what is shown then is a GPS-grounded figure, not
nonsense. If a real log shows good fronts being thrown out, that is the
filter to tune, not this.

### Dead wheel sensors

`core::fusion::WheelCheck` judges each wheel from the filter:

| Verdict | When | Health table |
|---|---|---|
| Unknown | nothing cross-checked yet (parked since key-on, no GPS) | Absent |
| Ok | read a plausible speed while moving | Ok |
| Dead | read zero, and rejected, for **5 s straight** at 5+ mph | Fault |

Five seconds is longer than any skid or locked wheel, so a long rear slide
or a stoppie does not fault a good sensor; a spin reads high, not zero,
and never counts. A Dead wheel is Ok again the first time it reads a
plausible speed, so a connector that comes back clears itself.

`logic` reports the verdicts as `FrontWheel` and `RearWheel` in the health
table, so they show on DIAG → Sensors and on the cruise fault line. `ctrl`
skips a faulted wheel when it picks the speedo's source (above), so a dead
front no longer means 0 mph for the rest of the ride -- nor, now, for the 5 s
before it is called dead, while the filter is already rejecting it.

**It only ever knows while moving.** On the board as drawn (netlist
2026-10-07), each Hall input is CNx pin 2 with a 10 kΩ pull-up to 3.3 V
(R59/R60), 10 nF to ground (C17/C18), an SMBJ10A clamp (D9/D8) and 10 kΩ in
series to the GPIO (R61/R62). The sensor is an Allegro A1120LUA-T, a
unipolar Hall switch with an open-drain output: on (LOW) while a magnet's
south pole is at its branded face, off (HIGH) otherwise. So an unplugged
sensor reads HIGH, which is also what a plugged-in one reads with no magnet
in front of it, and a wheel parked between magnets stays HIGH.
A LOW does prove a sensor is there (or the line is shorted to ground), but
HIGH proves nothing, so a parked dash cannot tell "unplugged" from "not
turning". Detecting it while parked needs hardware: a sense resistor on the
sensor's VIN feed (pin 1) into an ADC, so a missing sensor shows as no
supply current. That is a board change for the next revision.

### The known weak spot

With no front wheel to compare against, a rear spin smaller than the GPS
referee's allowance (a quarter of GPS speed) gets through. The
`spinnofront` scenario bounds it at about 9 mph peak. A working front
sensor removes it.

## Replay and tuning

Every blackbox row now carries the raw sources: `FrontMPH, RearMPH, GpsMPH,
HDOP, GpsUtc` and the dash's own `FusedMPH, FusedSigma`, at the end where
RaceRender ignores them. **Log at 50 Hz** (CONFIG → Log rate) for a ride
worth replaying.

```bash
cmake --build build-host                        # builds build-host/replay
build-host/replay --scenario all                # synthetic rides, known truth
build-host/replay --scenario wheelspin --out spin.csv
build-host/replay ride.csv --out ride_fused.csv --r-wheel 0.8 --gate 5
python3 tools/replay/plot.py ride_fused.csv     # writes ride_fused.svg
```

`replay` runs the firmware's own `SpeedFilter`, so settings that work there
work on the bike. Every `SpeedConfig` field it exposes is a command-line
option. The synthetic rides are in `tools/replay/scenarios.hpp`, shared with
`test/test_fusion.cpp`, so a number printed by `--scenario all` is the one
the tests hold the filter to.

The scenario noise (vibration, 0.04 g mount bias, wheel jitter, GPS 100 ms
late) is **a guess**. The first real 50 Hz log should replace it, and the
`SpeedConfig` defaults should be re-tuned against that log, not against the
scenarios.

| Scenario (8 seeds, worst) | Dash today, rms mph | Fused rms | Fused max |
|---|---:|---:|---:|
| ride | 0.24 | 0.11 | 0.82 |
| wheelspin | 0.24 | 0.15 | 1.07 |
| frontlock | 9.72 | 0.12 | 0.91 |
| gpsdropout | 0.24 | 0.11 | 0.78 |
| deadfront | 30.28 | 0.26 | 3.52 |
| parked | 0.00 | 0.02 | 0.10 |
| nogps | 0.23 | 0.11 | 0.81 |
| gpsglitch | 0.24 | 0.11 | 0.82 |
| spinnofront | 30.28 | 1.46 | 8.71 |
