# Dashboard v1 — netlist review and verified pin map

Source: `FInal_v1.net`. 167 parts, 110 nets.

**Electrically the netlist is clean.** No pin appears on two nets. No net
has fewer than two pins. No dangling connections. Both switching
regulators calculate correctly. Every chip-select has a pull-up. That is a
good result for a first PCB.

Findings below are ordered by what can stop the board working.

---

## 1. Before you power the panel: check FPC pins 1-8

This is the highest-risk item on the board, and it is a datasheet read,
not a rework.

Current state of the 40-pin display connector:

| FPC pin | Net | FPC pin | Net |
|---|---|---|---|
| 1 | **unconnected** | 13 | TFT_MOSI |
| 2 | **unconnected** | 14 | TFT_MISO |
| 3 | **unconnected** | 15 | TFT_RST |
| 4 | **unconnected** | 16-32 | GND |
| 5 | GND | 33 | LEDA |
| 6 | VCC | 34-36 | LEDK1/2/3 |
| 7 | VCC | 37 | GND |
| 8 | **unconnected** | 38 | 10k pull-up (R28) |
| 9 | TFT_CS | 39 | 10k pull-up (R29) |
| 10 | TFT_SCLK | 40 | 10k pull-up (R30) |
| 11 | DC/RS | 41, 42 | GND (tabs) |
| 12 | 10k pull-up (R2) | | |

Pins 9, 10, 11, 13, 14, 15, 33, 34, 35, 36 all match your datasheet
reading exactly. That part is right.

The concern is **pin 8, sitting unconnected between two pins tied to
VCC.** That is the exact pattern of a missed IM strap. The ILI9488 needs
IM2/IM1/IM0 = 111 to select 4-line SPI. If pins 6, 7, 8 are the IM pins
and pin 8 floats, the panel comes up in the wrong interface mode and
**never answers SPI at all** — which will look exactly like a dead panel
or a bad solder joint, and you will spend a day on it.

The other reading is that 6 and 7 are VCI and IOVCC (they are usually
adjacent) and pin 8 is a real NC. Equally plausible from the netlist
alone.

Same question for pins 1-4, which are unconnected. On this panel family
they are usually the 4-wire resistive touch lines, which is fine to leave
open. Confirm.

**Do this:** open the datasheet, write the function of pins 1-8, 12 and
38-40 into a comment block in `bsp/pins.hpp`, and confirm the IM straps
before you fit the panel. Ten minutes now against a day of bring-up.

While you are there, confirm that none of pins 16-32 is `RD`. In SPI mode
`RD` must be tied **high**, not grounded, and that whole block is
currently on GND.

---

## 2. U12 and U13 are in the BOM with zero connections

Both TLE4935LHALA1 Hall sensors appear in the parts list and connect to
nothing. You deleted them once already, and they are back.

They will be quoted, bought, and possibly placed. Delete them from the
schematic. This is the only outright error in the netlist.

---

## 3. Firmware cannot tell bike power from USB power

There is no VIN sense divider to an ADC, and the TPS2117 `ST` pin goes
only to LED10 — not to a GPIO. So the ESP32 has no way to know:

- whether the bike is running or the board is on the bench,
- what the battery voltage is,
- that the ignition just went off.

Three consequences for the firmware architecture:

1. The `Elec` state slot has nothing to fill it. Battery voltage cannot be
   displayed on the STAT screen.
2. The blackbox cannot flush on power loss.
3. The USB mass-storage handover has to rely on "USB enumerated and RPM is
   zero" rather than a real power-source test.

**The fix on the next revision is one divider to IO6.** That matters,
because of your three clean spare pins, IO6 is the only one on ADC1 —
IO38 and IO39 have no ADC at all. Use a 100k/22k divider: 14 V gives
2.5 V, and the SMBJ18A clamp at 29 V gives 5.2 V, so add a 3.3 V clamp or
raise the top resistor.

---

## 4. LED10 on the TPS2117 status pin probably will not light

`U10-8` (ST) drives LED10, then R52 200 Ω to GND. The LED needs ST to
**source** current.

The TPS2117 ST pin is an open-drain output. An open-drain pin can only
sink. Verify on the datasheet; if it is open-drain, wire it the other
way — VCC, then R52, then LED10, with the cathode on ST — and the LED
lights when ST pulls low.

Cosmetic, but it is a debug indicator, so a debug indicator that never
lights is worse than none.

---

## 5. Bench current on USB may exceed the port limit

D7 feeds `BIKEBATT_5V` from VUSB, so on the bench everything runs off the
USB port. Rough tally at 5 V:

| Load | Current |
|---|---|
| 8 shift lamps, 150 Ω each, all on | ~160 mA |
| Backlight, 3 strings at 60 Ω | ~80 mA |
| Wheel Hall sensors | ~20 mA |
| 3.3 V rail via AMS1117, ~500 mA out | ~350 mA in |
| **Total** | **~610 mA** |

A PC USB 2.0 port gives 500 mA. You will trip it with all eight shift
lamps on.

Not a hardware fault — but put a **USB power cap in firmware**: when
running without the bike, clamp the `SHIFT_POWER_PWM` duty on IO16. You
have that PWM already, so it costs nothing. It also solves the AMS1117
heat: 500 mA at 1.7 V drop is 0.85 W in a SOT-223, which runs hot.

---

## 6. Check the voltage rating on C22 and C23

`C22` (10 µF, 5 mm can) and `C23` (100 µF, 6.3 mm can) sit on VIN. The
SMBJ18A clamps at about 29 V under load dump.

A 6.3 mm 100 µF is commonly a 25 V part. That is not enough. You want
**35 V or 50 V** on both. C26 and C31 were already confirmed as 50 V
ceramics — C22 and C23 need the same check.

---

## 7. Tach input has no filter cap

`TACH_SIG` is R34 100 k series, R43 1 M bias, BAT54S clamp to VCC and GND.
The clamp is wired correctly — pin 1 GND, pin 2 VCC, pin 3 signal, which
is the right BAT54S series arrangement.

What is missing is a capacitor. An inductive pickup on an ignition lead
produces a damped ringing burst, not a clean pulse, so the GPIO will see
several edges per spark. A **470 pF from TACH_SIG to GND** gives a 47 µs
roll-off with R34 — far faster than the 6 ms pulse period at 10000 RPM,
but slow enough to swallow the ringing.

You can also fix this in firmware with a hard refractory window after each
accepted edge. Do that regardless, but the cap is one part.

---

## 8. Run the handlebar I2C at 100 kHz, not 400 kHz

`CN1` carries SDA, SCL, VCC, GND with R35/R36 4.7 k pull-ups. Over 0.5 m
of cable at roughly 100 pF, the rise time is about 470 ns. Fast mode
allows 300 ns. Standard mode allows 1000 ns.

So **100 kHz works, 400 kHz is out of spec.** Set it in `bsp/buses.cpp`
and write the reason in a comment.

Also note: `CN1` has no interrupt line. The pod **must be polled** — which
is what the architecture already does at 100 Hz. That is now confirmed by
the hardware rather than assumed.

---

## 9. What is right, so you do not go looking

- **Both rails calculate exactly.** 3V3 = 0.6 × (1 + 100k/22.1k) =
  **3.315 V**. 5V = 0.6 × (1 + 162k/22.1k) = **4.998 V**.
- **Every chip select has a 10 k pull-up** — R12 SD, R40 IMU, R41 TFT,
  R42 MCP. No bus contention before firmware runs.
- **Every FET gate has a pull-down** — R19-R26 on the shift lamps, R55 on
  Q11, R32 on the backlight. The board boots dark. Correct.
- **The two-FET shift-lamp topology is right.** 5 V → LED → 150 Ω → Q1-Q8
  → SHIFT_CTL → Q11 → GND. Q11 sinks ~176 mA through 40 mΩ, so 7 mV,
  which leaves Q1-Q8 with essentially full 3.3 V gate drive.
- **USB is correct end to end.** USB-C A6/A7 are D+/D−, they pass through
  the USBLC6 on the connector side, and land on IO20/IO19 — which are the
  ESP32-S3's dedicated USB pins.
- **SD card has all four pull-ups** (DAT0-DAT3/CMD) plus R14 22 Ω series
  on the clock.
- **PSRAM pins IO35/36/37 are correctly left open** on the N16R8.
- **Neutral opto works out at ~8 mA** through R6 1.5 k and the 1N4007,
  well inside PC817B range. Note for firmware: **neutral reads LOW.**
- **Rail LEDs are current-matched** — 6 mA, 5.4 mA, 6.5 mA across the
  12 V, 5 V and 3.3 V indicators, so they will look the same brightness.
- **The slide switch has both poles paralleled**, doubling its rating
  against roughly 260 mA of board draw.

---

## 10. Verified pin map

This replaces the provisional table in the architecture document.

### ESP32-S3-WROOM-1 (U2)

| Module pin | GPIO | Net | Function |
|---|---|---|---|
| 4 | IO4 | GPS_TX | UART RX from GPS |
| 5 | IO5 | GPS_RX | UART TX to GPS |
| 7 | IO7 | DC/RS | Display data/command |
| 8 | IO15 | TFT_BL | Backlight PWM (LEDC) |
| 9 | IO16 | SHIFT_POWER_PWM_3V3 | Shift-lamp master PWM |
| 10 | IO17 | TACH_SIG | Tach pulse input |
| 11 | IO18 | PPS | GPS pulse per second |
| 12 | IO8 | IMU_DATAREADY | ICM-42688-P INT1 |
| 13 | IO19 | D− | USB |
| 14 | IO20 | D+ | USB |
| 17 | IO9 | TFT_SCLK | Display SPI clock |
| 18 | IO10 | TFT_MOSI | Display SPI data |
| 19 | IO11 | MOSI | Shared SPI |
| 20 | IO12 | SCLK | Shared SPI |
| 21 | IO13 | MISO | Shared SPI |
| 22 | IO14 | IMU_CS | |
| 23 | IO21 | SD_CS | |
| 24 | IO47 | MCP_CS | |
| 25 | IO48 | TFT_CS | |
| 27 | IO0 | FLASH | Boot button SW1 |
| 33 | IO40 | TFT_MISO | Display SPI read |
| 34 | IO41 | SDA | Handlebar pod I2C |
| 35 | IO42 | SCL | Handlebar pod I2C |
| 36 | IO44 | DEBUG_RX | Header H1 |
| 37 | IO43 | DEBUG_TX | Header H1 |
| 38 | IO2 | WHEEL_HALL_R | Rear wheel |
| 39 | IO1 | WHEEL_HALL_F | Front wheel |

**Spare pins:**

| GPIO | Notes |
|---|---|
| IO6 | Clean, and **ADC1_CH5** — the only spare that can read a voltage |
| IO38 | Clean, no ADC |
| IO39 | Clean, no ADC |
| IO3 | Strapping (JTAG select), usable with care |
| IO45 | Strapping (VDD_SPI), must be low at reset |
| IO46 | Strapping (boot mode), must be low at reset |
| IO35/36/37 | **Do not use.** Octal PSRAM on the N16R8. |

Note IO1 and IO2 are ADC1 channels being used as digital inputs. That is
fine, but it means the two easiest analogue pins are already spent — which
is why the VIN divider in §3 has to go to IO6.

### MCP23S17 (U5), SPI address 0b000

| Pin | Port | Net |
|---|---|---|
| 21-28 | GPA0-GPA7 | SHIFT0-SHIFT7 |
| 6 | GPB5 | Neutral input, active LOW, R64 pull-up |
| 7 | GPB6 | TFT_RST, R3 pull-up |
| 8 | GPB7 | SD_CARD_DETECT, R16 pull-up |
| 1-5 | GPB0-GPB4 | **free** |
| 19, 20 | INTB, INTA | free |

A0/A1/A2 are grounded and RESET is tied to VCC.

---

## 11. One correction to the architecture document

I wrote that the display "cannot reset until the SPI expander works" and
called it a hard boot dependency. That was too strong.

`R3` is a 10 k pull-up on TFT_RST, so the panel releases itself from reset
whether or not the MCP23S17 ever initialises. A dead expander does not
leave the display bricked.

What is still true: firmware cannot issue a **controlled** reset pulse to
the panel without the MCP working, and the ILI9488 wants a proper hardware
reset at power-up rather than relying on its internal POR. So the boot
order stays MCP first, then panel — but a failed MCP is now a degraded
mode, not a dead screen.

---

## 12. Summary

| # | Item | Severity | Cost |
|---|---|---|---|
| 1 | FPC pins 1-8 unverified, pin 8 floats between two VCC pins | Highest | Datasheet read |
| 2 | U12/U13 in BOM with no connections | Medium | Delete from schematic |
| 3 | No VIN sense — firmware is blind to power source | Medium | 2 resistors to IO6, next rev |
| 4 | LED10 on open-drain ST will not light | Low | Flip the LED, next rev |
| 5 | USB bench current ~610 mA vs 500 mA port | Low | Firmware PWM cap |
| 6 | C22/C23 voltage rating unverified | Medium | Datasheet read |
| 7 | No tach filter cap | Low | 470 pF, next rev; firmware now |
| 8 | Pod I2C must run at 100 kHz | Low | One line of firmware |

Items 1 and 6 are datasheet reads and cost nothing. Item 1 decides whether
the display works at all — do it first.
