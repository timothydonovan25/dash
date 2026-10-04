# S3 respin — ILI9488 on an 8-bit parallel bus

**Status.** A plan, not a design. Nothing here has been drawn, ordered or
tested. The shipped S3 board (`Sheet_1_2026-09-23.net`, 167 parts, 110
nets — the same counts as `FInal_v1.net`) is still the platform, and
`docs/bringup.md` is still what happens next. This document exists so the
reasoning survives until a respin is due.

It is the cheap fallback named in `p4-platform-selection.md` §1: fix the
frame rate on the chip already in hand. It does not bring video, a
brighter panel or CAN.

---

## 1. Why

The panel is limited by its wire, not by the chip. A full 480×320 frame at the ILI9488's
SPI-mode RGB666 is 460 kB, 92 ms at 40 MHz — 11 fps whatever the drawing
costs. Every bit of dirty-rect machinery in `ui/` exists to work around
that number.

On the 8080 parallel interface the ILI9488 accepts RGB565, so a frame is
307 kB, two writes per pixel on an 8-bit bus. The datasheet's write cycle
is roughly 66 ns (**confirm against the real datasheet**), which puts a
full frame near 20 ms — about 50 fps of headroom for a 30 Hz `render`.

16-bit would halve that again. It does not fit the pin budget (§3) and
is not needed.

---

## 2. Step one: prove the panel can do it

The board is the easy half. The **panel module** has to bring the
parallel pins out to its FPC. Its specification (HT035SHV007NS,
`3.5寸TN_规格书_ILI9488_40PIN.pdf`) says it does: 8080 8/16-bit and
3/4-wire SPI, with DB0–DB7 on FPC 17–24 and DB8–DB15 on 25–32. Nothing
has run it in parallel yet.

FPC pins 16–32 are all on GND in the current netlist, as the datasheet
asks of unused data lines. On this board they are 0.5 mm pads tied into a
ground pour under the connector, so the current PCB cannot be bodged —
there is nothing to cut.

Before any layout:

1. Buy a 40-pin 0.5 mm FPC breakout. Plug the panel into it.
2. Wire it to an ESP32-S3 dev kit and flash `panel_test/`. It runs the
   panel in 8-bit 8080 mode on the §3 pin map, or in SPI mode as a
   baseline, and its README says what each test picture should look like
   and what a wrong one means.
3. Wire it from the panel datasheet. `panel_test/README.md` has the table.
   The parts that differ from the SPI board:
   - **FPC 10 is RS (D/C) and FPC 11 is WR in 8080.** In SPI they are SCL
     and D/CX. The pins keep their positions and swap roles.
   - RD is FPC 12, already pulled to IOVCC by R2. That is what the
     datasheet asks for when RD is unused.
   - IM2/IM1/IM0 are FPC 40/39/38. **011** is 8-bit 8080 ("DBI Type B
     8-bit"); today's 111 is 4-line SPI.
   - FPC 6–7 are IOVCC and FPC 8 is FMARK, the tearing-effect output,
     "open if not used". That closes `dash-v1-netlist-review.md` §1: pin 8
     floating between two supply pins is correct, not a missed strap. FPC
     1–4 are resistive-touch lines and correctly open.
4. Measure a full-frame write. That number decides whether dirty rects
   can go (§6).

If the panel will not run in 8080 despite the datasheet, the plan stops
here, having cost a breakout board instead of a PCB run.

---

## 3. Pin budget

8080 8-bit needs D0–D7, WR, DC and CS: 11 GPIO against SPI's 5. RD is
tied high, so the panel never drives the data lines. Reset stays on the
MCP23S17.

| Signal | GPIO | Today |
|---|---|---|
| WR | IO9 | TFT_SCLK |
| DC | IO7 | DC/RS, unchanged |
| CS | IO48 | TFT_CS, unchanged |
| D0 | IO10 | TFT_MOSI |
| D1 | IO40 | TFT_MISO |
| D2 | IO38 | spare |
| D3 | IO39 | spare |
| D4 | IO18 | GPS PPS, wired and unused |
| D5 | IO3 | strapping (JTAG select) |
| D6 | IO45 | strapping (VDD_SPI) |
| D7 | IO46 | strapping (boot mode) |
| RD | — | pull-up to VCC |
| RST | MCP23S17 GPB6 | unchanged |

LCD_CAM's i80 output goes through the GPIO matrix, so any GPIO works and
the assignment above is for routing convenience, not capability.

**Kept:** IO6 for VIN sense (§5.1), the debug UART on IO43/44, every
sensor, the shift lamps, tach, Halls and the pod.

### 3.1 What is lost

| Pin | Used today? | Cost |
|---|---|---|
| IO40, TFT_MISO | Configured in `bsp/src/buses.cpp`; nothing reads | No panel readback — no ID or status read, so a health check cannot tell a dead panel from a live one. Theoretical today. |
| IO18, GPS PPS | No | No precise GPS time later. Lap timing interpolates between fixes and does not need it. |
| IO38, IO39 | No | **No spare digital GPIO left.** The next feature goes on MCP23S17 GPB0–GPB4 (slow signals only) or takes IO6. |
| IO3, IO45, IO46 | No | Boot risk, below. |

### 3.2 The strapping pins

With RD high the panel's data pins are inputs, so they do not pull the
straps. The ESP only drives them after boot. That makes them safe, with
one rule: **put nothing on IO45 that pulls it high at reset.** IO45 high
selects 1.8 V for the flash supply, and a 3.3 V-flash N16R8 does not boot.
No pull-up, no test LED, no strong panel-side bias.

If that is not acceptable, put D5/D6 on IO43/IO44 instead and give up the
debug UART. The USB console on IO19/20 remains.

---

## 4. PCB changes for the display

- DB0–DB7 (FPC 17–24) to the pins in §3. Take them off GND.
- DB8–DB15 (FPC 25–32) stay on GND, as the datasheet asks for unused lines.
- RD (FPC 12) stays on R2's pull-up.
- **WR to FPC 11 and D/C to FPC 10**: the reverse of today's SPI nets on
  those two pins.
- FPC 13 (SDI) to GND, FPC 14 (SDO) open.
- IM2 (FPC 40) to GND: R30 becomes a pull-down or a 0 Ω to GND. IM1 and
  IM0 stay on R29 and R28.
- Keep the backlight as it is: LEDA, LEDK1–3, Q10, `TFT_BL` on IO15.

---

## 5. Fold in while the board is open

A respin is the cheap moment for everything else waiting on one.

### 5.1 VIN sense on IO6

**Not in the shipped netlist.** U2 pin 6 (IO6) is on no net. Firmware
cannot tell bike power from USB, has nothing to fill the `Elec` slot, and
cannot flush the blackbox at ignition-off (`dash-v1-netlist-review.md`
§3).

A 100k/22k divider from VIN to IO6 (ADC1_CH5): 14 V reads 2.5 V. The
SMBJ18A clamps at about 29 V, which the divider turns into 5.2 V — so add
a clamp to 3.3 V or raise the top resistor. A 100 nF at the ADC pin keeps
ignition noise out of the reading.

Take it from `VIN`, after F1 and the reverse diode, so it reads what the
regulators see.

### 5.2 Power section

Found on the bench, 2026-10-02, with the power section built and 12 V on
VIN:

- **The 3.3 V rail toggles between roughly 0 V and 5 V. Not yet
  diagnosed.** The netlist is correct for U3: pinout, CB, EN, the 3.31 V
  divider and the output caps all match the datasheet, and the LMR51430
  has no overvoltage protection to cause a cut-out. The leading suspect is
  a build fault tying `BIKEBATT_5V` to VCC, where U14 drives 5 V, U3 sinks
  to its −1.6 A limit and they hiccup. Settle it before copying the power
  section into a new layout.
- **L1 and L2 are 22 µH.** TI's 500 kHz table wants 5.6 µH for 3.3 V
  and 6.8 µH for 5 V. At 12 V in, 22 µH gives a 7–9 % ripple ratio
  against the 20–60 % that peak current mode needs for a clean sense
  signal.
- **D7 back-feeds U14 on USB alone.** It ties VUSB to `BIKEBATT_5V`, so
  with no bike power USB drives the 5 V buck's output, its high-side body
  diode lifts VIN to about 4 V, near the 3.89 V UVLO, and both bucks can
  cycle. The FPWM part (`XF` suffix) sinks current and makes it worse.
  Either keep USB off the buck output, or move to the PFM part
  (`LMR51430XDDCR`). The PFM part stops the active push but not the
  body-diode path.
- **Whether the 5 V rail survives at all** is open. It carries only the
  shift lamps, the backlight anode, the Hall supplies and LED12. The
  Halls move to 12 V for free. The lamps need about 560 Ω at 14.4 V or a
  constant-current sink. The backlight is three parallel strings, so it is
  not a single-string boost driver's load. Losing it also means a dark screen on USB
  alone unless the new driver's input is OR-ed from VUSB.

### 5.3 From the netlist review

| Review § | Item | Change |
|---|---|---|
| 2 | U12/U13 in the BOM with no connections | Delete from the schematic |
| 4 | LED10 on the TPS2117's open-drain ST cannot light | Flip it: VCC → resistor → LED → ST |
| 7 | No tach filter | 470 pF |

---

## 6. Firmware

| Component | Change |
|---|---|
| `bsp/pins.hpp` | New display block. PPS, `kSpareDigital0/1` and `kTftMiso` go. |
| `bsp/src/buses.cpp` | The SPI3 display bus becomes an i80 bus: `esp_lcd_new_i80_bus()` plus `esp_lcd_new_panel_io_i80()`, with DMA from PSRAM. |
| ILI9488 driver | RGB565 out, `bits_per_pixel = 16`. `esp_lcd_ili9488` 1.1.1 only speaks `esp_lcd_panel_io`, so it takes an i80 IO handle unchanged, and in 16-bit mode it sends pixels untouched. That is from reading its source; `panel_test/` is the run that proves it. Pixels go high byte first on the 8-bit bus. |
| `ui/` | **Keep dirty rects until the frame time is measured on real hardware.** If a full frame lands well inside 33 ms, they can be retired along with the "anything that moves must be a widget" rule and `test_ui`'s partial-versus-full check. |
| `core/`, `sim/` | None. |
| `CLAUDE.md` | "ILI9488 needs RGB666" becomes "in SPI mode"; the panel budget paragraph changes with the measured number. |

The real-time path is untouched. `render` stays on core 1, and the i80
DMA does not share a bus with anything on core 0.

---

## 7. Open

1. Whether the panel actually runs in 8080. Its datasheet says it does
   (§2); `panel_test/` is the proof.
2. ~~Which FPC pins are WR, RD, D/CX and IM.~~ Answered by the datasheet,
   §2 and §4.
3. The ILI9488's real write-cycle time, and the measured frame.
4. Whether `esp_lcd_ili9488` drives i80 in practice. Its source says yes
   (§6), but nothing has run it.
5. The 3.3 V rail fault (§5.2).
6. Whether the 5 V rail stays (§5.2).

---

## 8. Sequencing

Bring up the board in hand first (`docs/bringup.md`). It is the only
reference for the drivers, the panel stack and the real-time path, and a
respin before that leaves two untested boards and nothing to bisect
against.

Then the breakout test (§2), then layout. The breakout test can run in
parallel with bring-up — it needs a dev kit and the panel, not the board.
