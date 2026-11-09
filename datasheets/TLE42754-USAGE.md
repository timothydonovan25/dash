# TLE42754D (U5) — usage

Infineon automotive LDO, 5.0 V ± 2 %, up to 450 mA, 42 V in, TO-252-5.
Datasheet Rev. 1.31. Feeds **LEDA, the panel backlight anode, only** — from
VIN, so it rides out load dumps. Firmware doesn't touch it; the backlight
current is set by the PCA9955B's sinks.

## Circuit notes
- **Pin 3 is on no net** (the datasheet marks it NC); the part is grounded
  through its **tab, which must be soldered**.
- The reset output and delay pins are unused.
- Needs its output capacitor for loop stability.

## Thermal
At full backlight (90 mA), (14.4 − 5) × 0.09 ≈ 0.85–1.0 W. Warm at 70 °C
ambient; full brightness is daylight only, nights run a few percent.

## Headroom
- Must stay ≥ ~4.3 V: 3.5 V worst-case backlight string + 0.8 V PCA9955B
  headroom.
- On USB it drops out to ~4.2 V and a worst-case string dims slightly.
- Bring-up check: LEDA ≈ 5 V. Backlight dark → PCA9955B first, then LEDA,
  then FPC 33–36.
