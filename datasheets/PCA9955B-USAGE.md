# PCA9955B (U2) — usage

NXP PCA9955BTWJ, HTSSOP-28 with thermal pad (8 thermal vias to GND). 16
constant-current sinks, 8-bit IREF and 8-bit PWM per channel. Datasheet Rev.
2.3 (Sept 2024). Driver: `components/drivers/src/pca9955b.cpp`. Design:
`docs/p4-board-pin-map.md` §2.2–2.3.

## Wiring
| Pin | Connection |
|---|---|
| SDA / SCL | GPIO34 / GPIO0, **I2C1 at 1 MHz (Fm+)**, 2.2 kΩ pull-ups. Never shared with the pod bus |
| AD2/AD1/AD0 | GND/VDD/GND → **address 0x15** (0x01 is NXP-reserved) |
| OUT0–7 | Shift lamps LED0–7 (anodes on 3.3 V VCC, no resistors) |
| OUT8–10 | Backlight LEDK1–3 (FPC 34–36), anode LEDA = 5 V from U5 |
| OUT11–15 | Unused |
| REXT | 1.91 kΩ 1 % → full scale 255 × 0.225 / 1.91 = **30.0 mA** per output |
| OE | GND (always enabled) |
| RESET | `ESP_EN` — a software/panic reset does **not** reset it |

## Registers used
- MODE1 0x00, PWM0 0x08.., IREF0 0x18.., LEDOUTn (2 bits/output, `10b` =
  individual PWM; `00b` off for unused outputs).
- MODE2 0x01 (read: bit 7 OVERTEMP, bit 6 ERROR; written once a second
  with bit 4 CLRERR set) and EFLAG0–3 0x46–0x49 (2 bits/output: `01b`
  short, `10b` open), read by `read_faults()` just before that write, so
  each read covers the second since the last clear. Decoded in
  `core::shift::decode_led_faults`.
- Bit 7 of the register address = auto-increment; one transfer writes all
  eight lamp PWMs (~0.1–0.2 ms at 1 MHz).
- `init()` rewrites every register, because only a power-on / EN reset
  clears the chip.
- IREF 255 = 30 mA for lamps and for each backlight string — the hardware
  ceiling; `bsp::kLedLampIrefMax`, `kLedBacklightIrefMax`.

## Policy
- **`ctrl` is the only writer** (backlight level from `keeper` via
  `Deps::backlight_pct`); exception: `power` blanks it at VIN cut.
- On USB, `ctrl` caps lamp duty at `kShiftDutyUsbMax` (96/255).
- 2 ms I2C timeout: a wedged bus costs one 500 Hz tick, not five.
- Open/short detection is health, not policy (rule 5): `ctrl` publishes it
  in `Engine::led_faults`; DIAG → Bus shows it on the I2C1 row and `_evt`
  logs a `led_fault` line on each change.
- **The chip tests an output only while it is lit at a PWM of 8..254**
  (datasheet §7.3.15): not off, not below 8, not at 255. The driver's 100 %
  is therefore 254. A lamp is tested only once the rpm lights it.
- Open = current under half the target (Ith(det) = 0.5 × IO). Short = the
  output above Vtrip ≈ 2.85 V while on. A lamp (3.3 V anode, red Vf ≈ 2 V)
  sits near 1.3 V healthy and at VCC shorted, so both readings mean
  something. A backlight string (5 V anode, one ≈ 3 V LED) should sit near
  2 V, below Vtrip, but a low-Vf string could cross it: **backlight short
  is unproven until read on the bench**, so DIAG leaves it out and only
  `_evt` carries it.
- OVERTEMP (130–150 °C junction) blanks every output by itself, backlight
  included, and recovers on its own; `_evt` is the only place it shows.
- A dead PCA9955B = dark screen **and** dark lamps.

## Status
Backlight works (so the chip answers); shift lamps untested on the board.
Fault reading built (2.10), not yet run on the board: check a pulled lamp
reads open and a clean board reads clean, and read the backlight strings'
short bits.
