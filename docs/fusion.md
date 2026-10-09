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
  follow, because it would drag the estimate down with it.
- **Wheel split**: front and rear more than 2 mph apart means one is
  spinning or locked; only the one nearer the referee (or the filter) is
  used. That catches a spin that builds gently enough to slip through the
  gate.
- A source rejected for 1.5 s straight is **suspect**. If every offered
  source is suspect, the filter is the one that is wrong, and it restarts
  on GPS.
- With no correction for 2 s it stops claiming to be **valid**.

On screen: DIAG → GPS shows "Fused mph" with its 1σ and which sources are
in use (`F R G`, lower case = suspect). CONFIG → Display → Speedo picks
what the big number shows: Wheels (the default) or Fused. **Leave it on
Wheels until the filter has been tuned on real rides.**

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
