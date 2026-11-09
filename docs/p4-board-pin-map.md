# P4 board pin map — Guition JC-ESP32P4-M3-C6 (U16)

**Status.** Ordered 2026-10-07 from the package listed in §6. The board
implements this map as drawn (§2, rows 45–52 and 69–73 moved from the first
plan) and was audited before ordering (§6). It arrived on 2026-10-28; what has run on
it is in `bringup.md`'s status table. The module is a Guition JC-ESP32P4-M3-C6:
ESP32-P4NRW32 (32 MB in-package PSRAM, 16 MB flash) plus an
ESP32-C6-MINI-1U-N4 on SDIO, 27 × 27 mm, 80 castellated pins.

The panel runs on an **8-bit 8080 parallel bus**, not SPI. The panel's
datasheet (HT035SHV007NS, ILI9488, 40-pin FPC) lists 8080 8/16-bit and
3/4-wire SPI, and brings DB0–DB15 out on FPC 17–32. FPC pin functions in
§4 are taken from it. It runs in parallel on the first board, at a
25 MHz write clock with the IPS init sequence.

Sources: the module symbol and schematic in Guition's dev-board repo
(github.com/megavatt05/JC-ESP32P4-M3-DEV, `5-Schematic/`), the Espressif
ESP32-P4 datasheet v0.5 (strapping, IO MUX, power domains), IDF v6.0.2's
`soc_caps.h` for the P4 (`SOC_LCD_I80_SUPPORTED`), and the panel
specification `3.5寸TN_规格书_ILI9488_40PIN.pdf` (version 1.0).

---

## 1. Rules the assignment follows

| Avoid | Why |
|---|---|
| GPIO14–19 | Not brought out. They are the SDIO link to the C6. |
| GPIO6 (pin 10) | The dev board labels it `C6_IO2`. Treat it as wired to the C6 until proven otherwise. |
| GPIO39–48 (pins 59–68) | Powered by `VDD_IO_5`, which is fed from the on-chip LDO at pin 58 and set by firmware. Not a fixed 3.3 V bank. |
| GPIO35/36 (pins 54/55) | Boot straps. Download mode is GPIO35 = 0 **and** GPIO36 = 1. |
| GPIO37/38 (pins 56/57) | UART0, the ROM console. Kept for debug. |
| GPIO54 (pin 74) | The C6's reset line (`esp_hosted` reset). |

The i80 bus goes through the GPIO matrix, so any clean pin works. The data
lines sit on one contiguous run (pins 45–52) to keep them short and
matched; WR is the timing-critical line and sits next to them on the top
edge.

**Firmware note:** the display leaves SPI altogether
(`esp_lcd_new_i80_bus()`, RGB565, see `history/s3-parallel-respin.md` §6). The
shared sensor bus stays on SPI2, as in today's `bsp/buses.cpp`, through
the GPIO matrix. At 24 MHz or less that is comfortable.

---

## 2. The map

| Pin | GPIO | Net | Notes |
|---|---|---|---|
| 1, 3, 7, 25, 32, 41, 44, 79, 80 | — | GND | already wired |
| 2 | — | — | `LAN_OUT`: leave unconnected |
| 4 | GPIO1 | `SD_CARD_DETECT` | CARD1-9, keep R16's pull-up. Was MCP23S17 GPB7. |
| 5 | GPIO2 | `GPS_TX` | GPS TX → P4 RX (UART1) |
| 6 | GPIO3 | `GPS_RX` | P4 TX → GPS RX |
| 8 | GPIO4 | `SDA` | pod I2C, 100 kHz |
| 9 | GPIO5 | `SCL` | |
| 10 | GPIO6 | — | **leave unconnected** (§1) |
| 11 | GPIO7 | `IMU_CS` | shared SPI (SPI2) |
| 12 | GPIO8 | `MOSI` | shared SPI |
| 13 | GPIO9 | `SCLK` | shared SPI |
| 14 | GPIO10 | `MISO` | shared SPI |
| 15 | GPIO11 | `SD_CS` | shared SPI |
| 16 | C6 U0RXD | test pad | C6 recovery flashing |
| 17 | C6 U0TXD | test pad | |
| 18 | C6 IO9 | 10 kΩ pull-up | C6 boot strap: high = normal boot. The dev board uses 5.1 kΩ; either holds it. |
| 19 | GPIO12 | `NEUTRAL` | opto U6 collector, keep R64's pull-up. **Reads LOW in neutral.** Was MCP23S17 GPB5. |
| 20 | GPIO13 | `IMU_DATAREADY` | ICM-42688-P INT1 |
| 21 | GPIO20 | `VIN_SENSE` (new) | ADC1_CH4. Divider, §3 |
| 22 | GPIO21 | `TACH_SIG` | pulse capture |
| 23 | GPIO22 | `WHEEL_HALL_F` | |
| 24 | GPIO23 | `WHEEL_HALL_R` | |
| 26–31 | — | — | MIPI-DSI, unused on this board |
| 33–38 | — | — | MIPI-CSI, unused on this board |
| 39 | — | — | high-speed USB OTG, not connected |
| 40 | — | — | |
| 42 | GPIO24 | `D-` | USB-Serial/JTAG on the USB-C, via D3: flash, console, JTAG |
| 43 | GPIO25 | `D+` | |
| 45 | GPIO26 | `TFT_D7` | i80 data. **The bus is reversed as drawn:** D7 on pin 45 down to D0 on pin 52 |
| 46 | GPIO27 | `TFT_D6` | |
| 47 | GPIO28 | `TFT_D5` | |
| 48 | GPIO29 | `TFT_D4` | |
| 49 | GPIO30 | `TFT_D3` | |
| 50 | GPIO31 | `TFT_D2` | |
| 51 | GPIO32 | `TFT_D1` | |
| 52 | GPIO33 | `TFT_D0` | |
| 53 | GPIO34 | `LED_SDA` | PCA9955B, own I2C bus at 1 MHz (§2.2). Strap pin, see §2.1. |
| 54 | GPIO35 | `FLASH` | BOOT button to GND + 10 kΩ pull-up. **Moved from pin 76.** |
| 55 | GPIO36 | 10 kΩ pull-up | required for download mode |
| 56 | GPIO37 | test pad `DEBUG_TX` | UART0 |
| 57 | GPIO38 | test pad `DEBUG_RX` | UART0 |
| 58 | — | — | `ESP_LDO_VO4`: leave unconnected |
| 59–68 | GPIO39–48 | — | leave unconnected (§1) |
| 69 | GPIO49 | `TFT_TE` | panel FMARK, **FPC 8**: tearing-effect output for tear-free updates. Optional. Was `TFT_BL`; the backlight is on the PCA9955B (§2.2). |
| 70 | GPIO50 | `TFT_CS` | **FPC 9**. Keep R21's 10 kΩ pull-up: holds CS off while the pin floats at boot |
| 71 | GPIO51 | `DC/RS` | **FPC 10** |
| 72 | GPIO52 | `TFT_WR` | i80 write strobe, through R30 (22 Ω) to **FPC 11** |
| 73 | GPIO53 | `TFT_RST` | **FPC 15**. Keep R3's pull-up. Was on the MCP23S17 on the S3 board. Now a plain GPIO, which removes the "expander before panel" trap from `bringup.md`. |
| 74 | GPIO54 | C6 reset | 10 kΩ pull-up + 1 µF to GND, as the dev board's R81/C64 |
| 75 | CHIP_PU | `ESP_EN` | R27, C30, SW3 already on the net |
| 76 | GPIO0 | `LED_SCL` | PCA9955B. `FLASH` was here; GPIO0 is not a boot pin. |
| 77, 78 | — | `VCC` | 3.3 V, already wired |

Rows 45–52 and 69–73 are as drawn (netlist 2026-10-06 rev 5), not as first
planned: the layout reversed the data bus and moved the control pins when
the panel connector was changed (§6.1). All of them are plain GPIOs and the
i80 bus takes any GPIO for any bit, so `pins.hpp` follows the board.

**No MCP23S17.** Everything it did has a home: the shift-lamp gates went
to the PCA9955B, `TFT_RST` to GPIO53, and `NEUTRAL` and
`SD_CARD_DETECT` to GPIO12 and GPIO1. The shared SPI bus now carries only
the IMU and the SD card.

**Gone:** `PPS`, `TFT_MISO`, and `TFT_SCLK`/`TFT_MOSI` as SPI nets. RD is
tied high, so nothing reads the panel back. `SHIFT_POWER_PWM_3V3`,
`SHIFT_CTL`, `SHIFT0`–`7`, `SHIFT_POWER0`–`7` and `TFT_BL` go with the FET
circuits (§2.2). `MCP_CS` goes with U5.

### 2.1 Spares

None outright. **GPIO49** (pin 69) is free unless `TFT_TE` is used. The
panel drives FMARK once per refresh, so `render` can start a frame just
after the panel's scan passes, and a moving widget does not tear. The
datasheet says to leave FMARK open if unused, so dropping it costs
nothing but that.

Two inputs could give up their pins if something more important needs one.
**`SD_CARD_DETECT`**: firmware can find a missing card by the mount failing,
and the slot then needs no detect line. **`IMU_DATAREADY`**: the imu
service already falls back to polling. Anything beyond that has to come
from the `VDD_IO_5` bank, and only for slow, non-boot-critical signals,
after firmware has set the LDO at pin 58 to 3.3 V.

**GPIO34** carries `LED_SDA`. It is the JTAG-source strap, but the
datasheet only reads it when the `JTAG_SEL_ENABLE` eFuse is burnt, and it
is not by default. The bus pull-up holds it high at reset, which is
harmless.

### 2.2 PCA9955B (U2): shift lamps and backlight

NXP PCA9955BTWJ (LCSC C2678563), HTSSOP-28 with a thermal pad: 16
constant-current sinks, up to 57 mA, each with its own 8-bit current and
8-bit PWM. It replaces Q1–Q8, Q10, Q11, R17/R18/R39/R44–R48, R19–R26,
R31, R32, R55 and the backlight's 30 Ω pairs.

| PCA9955B pin | Connects to | Notes |
|---|---|---|
| SDA / SCL | `LED_SDA` (GPIO34) / `LED_SCL` (GPIO0) | **its own bus**, not the pod bus. 2.2 kΩ pull-ups to VCC for 1 MHz. |
| OUT0–OUT7 | LED0–LED7 cathodes | shift lamps, **anodes on VCC (3.3 V)**, no series resistors |
| OUT8–OUT10 | `LEDK1`–`LEDK3` (FPC 34–36) | backlight, one cathode pin per channel, no ballast resistors. The panel is specified at 90 mA total, so 30 mA per pin: exactly REXT's full scale. |
| OUT11–OUT15 | — | spare, leave open |
| `LEDA` (FPC 33) | 5 V LDO from `VIN` (§2.3) | replaces R63 |
| VDD (28) | VCC | 3–5.5 V. Plus 100 nF and 1 µF at the pin. |
| REXT (1) | **1.91 kΩ 1 %** to GND | full scale 255 × 0.225 / 1.91 = **30.0 mA**; 0.118 mA per IREF step. The lamps run at IREFx = 255; 20 mA is 170. Open/short detection needs REXT ≤ 3 kΩ. |
| AD2, AD1, AD0 (4, 3, 2) | **GND, VDD, GND** | 7-bit address **0x15** (0x2A write). All three on GND gives 0x01, which NXP flags as a reserved address. Tie directly; the pins must be stable before VDD. |
| OE (5) | GND | always enabled. No pin left for hardware flash; PWM does it. |
| RESET (25) | `ESP_EN` | the datasheet asks for an external 10 kΩ pull-up, and R27 on `ESP_EN` is one. A hardware reset of the P4 also clears the lamps. Firmware still sends SWRST at boot, because a panic restart does not toggle `ESP_EN`. |
| VSS (10, 19, 24), thermal pad | GND | the pad needs thermal vias to an inner ground plane |

Checked against the datasheet (Rev. 2.3, September 2024):

- **Headroom, lamps.** The driver needs 0.8 V minimum across each output
  (`Vreg(drv)`), but NXP only specifies it at 57 mA. A 30 mA output
  needs less, and the datasheet does not say how much. The LCSC reel of
  LR P47F (`-3A5A`) spans forward-voltage bins 1.80–2.55 V at 30 mA, and
  Vf rises about 0.12 V at −20 °C. With VCC at its 3.25 V low end:

  | Lamp | Vf at 30 mA | Headroom |
  |---|---|---|
  | Typical (bin 4A, 25 °C) | ~2.2 V | ~1.05 V |
  | Worst (bin 5A, 25 °C) | 2.55 V | 0.70 V |
  | Worst, −20 °C | ~2.67 V | ~0.58 V |

  Bins 3A–4B clear 0.8 V outright. A 5A-bin lamp falls below the
  57 mA figure, so at worst it regulates a little under 30 mA and is a
  little dimmer. It does not go dark. That is a smaller difference than
  the 3× spread already in the brightness bins (560–1800 mcd). Reaching
  50 mA would need a higher anode rail. A 4.2 V buck was costed and
  dropped as not worth a second converter.
- **Headroom, backlight:** the panel gives Vf 3.2 V typical, 3.5 V
  maximum. 5 − 3.5 = 1.5 V worst case. On USB the LDO drops out to
  about 4.2 V, leaving 0.7 V on a worst-case string, which is as marginal
  as the worst lamp and dims slightly in the same way.
- **Short detection** flags an output whose voltage is at or above
  2.85 V. Healthy lamps sit at 3.3 − Vf, about 0.6–1.5 V, and the
  backlight at about 1.8 V. A shorted LED pulls its output to the anode
  rail: 3.3 V for the lamps, at least 3.25 V, still above the threshold, and
  5 V for the backlight. On 12 V every healthy output would read as
  shorted.
- **Open detection** flags current below half the target, so a 5A-bin
  lamp running slightly short of 30 mA does not trip it.
- **Heat.** All eight lamps at 30 mA on the lowest-Vf parts is about
  (3.3 − 1.8) × 0.24 A ≈ 0.36 W, plus (5 − 3.0) × 0.09 A ≈ 0.18 W of
  backlight at full brightness: about 0.55 W, so +21 °C at 39 °C/W.
- **Outputs tolerate 20 V when off**, and the 5.5 V-tolerant logic pins
  are fine on 3.3 V.
- **I2C.** Fast-mode Plus at 1 MHz, with 30 mA SDA drive. 2.2 kΩ
  pull-ups give a rise time of about 40 ns on a short on-board bus,
  against Fm+'s 120 ns limit.
- **Automotive part.** PCA9955BTW/Q900 is the AEC-Q100 version, same
  pinout.

**Firmware:** `drivers/` gains a PCA9955B driver behind the shift-lamp and
backlight interfaces. `ctrl` writes the eight lamp PWM registers in one
auto-increment transfer, about 0.1 ms at 1 MHz, inside the 3 ms budget.
Once a second `ctrl` reads MODE2 and EFLAG0–3 (open, short, overtemperature)
and publishes them in `Engine::led_faults`; DIAG → Bus and `_evt` show them,
and nothing turns an output off over one (rule 5). The chip tests an output
only while it is lit at a PWM of 8..254, so the driver's 100 % is 254
(`datasheets/PCA9955B-USAGE.md`).

**REXT is shared by all sixteen outputs.** 1.91 kΩ caps every channel at
30 mA in hardware, which is both the lamps' chosen maximum and the
panel's specified 30 mA per backlight pin (90 mA total). No firmware bug can
push either past its rating. Put the per-channel ceilings in `bsp/`
anyway, as hardware facts next to the pin numbers, so a future REXT
change cannot silently overdrive the backlight.

### 2.3 Rails for the LEDs

- **Lamps: VCC, the existing 3.3 V buck.** 8 × 30 mA adds 0.24 A, well
  inside U3's 3 A. Lamp PWM is 31.25 kHz on the logic rail, so stagger
  the channels with the PCA9955B's programmable output delay. That way
  eight 30 mA steps do not land on VCC in the same instant.
- **Backlight: a 5 V LDO from `VIN` for `LEDA` alone.** 90 mA at full
  brightness, so (14.4 − 5) × 0.09 ≈ **0.85 W** in the LDO. In SOT-223 on
  a generous copper pour that is roughly +45 °C, which is warm at a 70 °C
  ambient. Daylight wants full brightness; night riding wants a few
  percent, so the average is much lower. It must take at least 40 V in to ride out
  load dumps (TPS7B69-Q1 class; LCSC stock not checked). Its output must
  not go below about 4.3 V, which is a 3.5 V worst-case string plus the driver's 0.8 V.
- **No second buck.** It was only needed for 50 mA.
- **USB:** about 0.24 A of lamps at 3.3 V plus the P4 and the backlight
  is close to a 500 mA PC port at full bar. Firmware caps lamp current
  when `VIN_SENSE` reads USB, which is the job `kShiftDutyUsbMax` does on
  the S3 board.

**Derating (OSRAM LR P47F, IF = f(T)):** the curve runs from 50 mA at
70 °C ambient down to 20 mA at 100 °C, so **30 mA is allowed up to about
90 °C ambient**. That is beyond the P4 module's own 70 °C rating, so the
module is the limit, not the lamps. Brightness at 30 mA is about 1.4×
that at 20 mA, and about 60 % of what 50 mA would give.

---

## 3. VIN sense

`VIN` → 100 kΩ (R17) → `VIN_SENSE` → 22.1 kΩ (R18) → GND, with 100 nF at the pin.

| VIN | Pin voltage |
|---|---|
| ~4.5 V (USB through D5) | 0.81 V |
| 12.6 V (engine off) | 2.28 V |
| 14.4 V (running) | 2.61 V |
| ~29 V (SMBJ18A clamping) | 5.2 V — **needs a clamp** |

Add a clamp to 3.3 V on `VIN_SENSE`: the BAT54S pattern used on the tach
input works. USB and bike power read 1.5 V apart, which is the
power-source test `history/dash-v1-netlist-review.md` §3 asked for.

---

## 4. Netlist changes this implies

This is the change list from the S3 board to the P4 board as first planned,
kept for the reasoning. Reference designators in it (R28, R29, R30, R63, Q10)
are the S3 board's. The drawn P4 board ties IM0/IM1 straight to VCC and IM2
to GND, and its panel connector is the flipped top-contact part in §6.1.

1. `FLASH`: U16-76 → **U16-54**. Add 10 kΩ to VCC on pin 54 and on pin 55.
2. `ESP_EN` → **U16-75**.
3. Pin 74: 10 kΩ to VCC, 1 µF to GND. Pin 18: 10 kΩ to VCC.
4. Test pads on pins 16, 17, 56, 57. Pins 42/43 go to the USB-C instead.
5. New net `VIN_SENSE` with the divider and clamp in §3.
6. Delete `PPS` (U4-13 left open) and `TFT_MISO`.
7. **FPC1, from the panel datasheet** (pin: function → net). The pin
   numbers in this table are the **panel's**. On the drawn board the
   connector is top-contact and the panel's tail arrives flipped, so panel
   pin n lands on connector pad 41−n (§6.1).

   | FPC | Function | Net | Change from today |
   |---|---|---|---|
   | 1–4 | resistive touch XL/YU/XR/YD | open | none |
   | 5, 16, 37 | GND | GND | none |
   | 6, 7 | IOVCC 2.8–3.3 V | VCC | none |
   | 8 | FMARK (tearing effect) | `TFT_TE` | new; open if not wired |
   | 9 | CS | `TFT_CS` | none, keep the pull-up (R21 as drawn) |
   | **10** | **RS (D/C)** in 8080; SCL in SPI | **`DC/RS`** | **was `TFT_SCLK`** |
   | **11** | **WR** in 8080; D/CX in 4-wire SPI | **`TFT_WR`** | **was `DC/RS`** |
   | 12 | RD, "fix to IOVCC when unused" | VCC via R2 | none |
   | 13 | SPI SDI, "fix to IOVCC or GND when unused" | GND | was `TFT_MOSI` |
   | 14 | SPI SDO, "open when unused" | open | was `TFT_MISO` |
   | 15 | RESET, active low | `TFT_RST` | none, keep R3 |
   | **17–24** | **DB0–DB7** | **`TFT_D0`–`TFT_D7`** | **were GND** |
   | 25–32 | DB8–DB15, "GND when unused" | GND | none |
   | 33 | LED-A | 5 V LDO | was `VIN` via R63 |
   | 34–36 | LED-K1–K3 | PCA9955B OUT8–10 | were the 30 Ω pairs and Q10 |
   | 38 | IM0 = 1 | VCC via R28 | none |
   | 39 | IM1 = 1 | VCC via R29 | none |
   | **40** | **IM2 = 0** | **GND** | **R30 was a pull-up** |

   IM2 IM1 IM0 = **011** is "DBI Type B 8-bit", data on DB[7:0], per the
   panel's Table 3. Today's 111 is 4-line SPI. Watch rows 10 and 11: the
   pins keep their positions but swap roles between SPI and 8080, so
   copying the SPI net names across puts D/C and WR the wrong way round.
8. **U2 (PCA9955B), §2.2:** wire it as tabled, with REXT = 1.91 kΩ. Move
   the LED0–LED7 anodes from `VIN` to VCC. Put `LEDA` on a new 5 V LDO
   (§2.3) instead of R63. Delete the FET shift-lamp and backlight circuits
   listed there.
9. **Delete U5 (MCP23S17)**, R42 (its CS pull-up) and `MCP_CS`. C32
   (10 µF) and C36 (100 nF) sit with U5's supply pins in the netlist and
   look like its local decoupling. Delete them if the layout confirms
   that. Route `NEUTRAL` (U6-4, R64) to U16-19 and `SD_CARD_DETECT`
   (CARD1-9, R16) to U16-4.
10. Everything else in §2 is a wire from an existing net to a U16 pin.

Still open from the netlist review: VCC has no source until
`BIKEBATT_3V3` is merged into it. Item 8 also fixes `LEDA`, which is on
`VIN` through 60 Ω per string today.

---

## 5. Module risks

- **−20 to +70 °C.** `history/p4-platform-selection.md` §7 expects a dark dash in
  sun to reach 70–80 °C before the backlight. That is at or above this
  module's rating. The panel is rated the same: −20 to +70 °C operating,
  −30 to +80 °C storage.
- **Chip revision.** The P4 demo board was revision v1.3, which needs
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Read it with esptool on arrival.
- **Antenna.** The C6 is the MINI-1U, with an IPEX connector. It needs an
  external antenna routed out of the enclosure.

---

## 6. Audit of the drawn board (2026-10-06)

The netlist, PCB and gerbers were audited before ordering
in four parts: power, MCU and buses, bike inputs, and layout/DFM/BOM.
What follows is what the audit found and what was decided. Nothing here
has been measured.

**Ordered 2026-10-07** from JLCPCB: 4 layers, 1.6 mm FR-4 TG155, stackup
JLC04161H-7628, ENIG, epoxy filled and capped vias, top side assembled,
depanelled. Hand-fitted on arrival: U16 and U4 (not in the BOM), the 1 A
fuse in F1, the bottom-side LED0–LED7, and any through-hole part
deselected at assembly (SW2, U8, U11).

**As ordered:** the files JLCPCB received are in `v2 pcb files/` at the
repository root — gerbers generated 2026-10-07 00:53:17, the BOM, and the
pick-and-place file. They come from PCB `PCB_PCB_dash-v1_2026-10-07` and
netlist `Sheet_1_2026-10-07` (identical to rev 5), and were checked against
both:

- all 344 via drills match the PCB to 0.002 mm, and every via has a net;
- BOM: 112 placed parts, about $31.50, FPC1 = AFC07-S40ECA-00 (C262648),
  C23 = C48971006, with U4, U16 and H1–H10 deliberately left out;
- the top and bottom GND pours (270 and 79 regions) were rasterised and no
  copper of another net lies inside them; the 00:33 export checked first
  had those pours cleared, and is otherwise identical shape for shape;
- no tracks or vias of different nets touch anywhere; the closest pair is
  the routed USB D+/D− at 0.10 mm. DRC leaves only connector-pitch pad
  gaps, the USB-C stubs, necked-down power stubs and the copper smiley.

The FPC1 re-route of PCB rev 6 had shorted VIN to GND (a bottom-layer VIN
trace under the connector's via field) among others; the ordered revision
clears it.

### 6.1 Resolved

- **Panel connector was bottom-contact.** The panel sits on the bottom
  side and its FPC wraps round the right board edge to FPC1 on the top, so
  the fingers arrive facing up and in reverse order — confirmed with a card
  mock-up. AFC01-S40FCA-00 (bottom contact) was replaced by **AFC07-S40ECA-00
  (C262648, top contact, slide lock, H2.0)**, and the netlist maps panel pin
  n to connector pad 41−n: pad 1 GND (IM2), 2–3 VCC (IM1, IM0), 5–8
  LEDK3..LEDK1, LEDA, 17–24 D7..D0, 26 RST, 29 RD (R2), 30 WR, 31 RS,
  32 CS, 33 TE, 34–35 VCC, 37–40 open (touch). Pad 1 sits at the bottom
  end, opening to the right edge (PCB rev 6). Checked pin by pin, and
  IM[2:0] = 011, 8-bit 8080.
- **TFT control pins** (superseded by the connector change, rows 45–52 and
  69–73 now as drawn). Earlier the board had U16-69..72 as RST, WR, DC, CS; this
  document and `pins.hpp` had WR, DC, CS, RST. Flashed as it was, the i80
  write strobe would have driven the panel's reset. Fixed in firmware
  (§2 rows 69–72, `pins.hpp`), no layout change.
- **F1 is a fuse holder only** (XF-508PVS-B). The BOM has no fuse in it.
  A **1 A** fuse is fitted by hand. Worst-case input is about 0.45 A at
  12 V, so 1 A clears normal running and still opens on a harness short.
- **U16 and U4 are not in the BOM** and are hand-fitted after assembly:
  the JC-ESP32P4-M3-C6 (80 castellated pins) and the PA1616D (LCC-20).
  JLC ships the board without them.
- **Buck output.** VCC = 0.6 V × (1 + 100k / 22.1k) = **3.31 V**. The
  LMR51430's reference is 0.6 V. An 0.8 V figure gives 4.42 V and was
  wrong.
- **C6 antenna.** The C6 is the MINI-1U with its own IPEX, so the module
  needs no copper keep-out. It does still need an external antenna out of
  the enclosure (§5).

- **Hold-up on VIN.** C23 went from 100 µF to **220 µF** (netlist
  2026-10-06 rev 3, footprint now `CAP-SMD_BD6.3-L6.6-W6.6-LS7.3-FD`),
  the largest 35 V part in the same 6.3 mm can: JIERR RV35V220M6X8
  (LCSC C48971006) or JVJ35V220M6x8 (C46550411). The BOM line must be
  **35 V or more** — D4 lets transients reach about 29 V. About 236 µF on
  VIN gives roughly 9 ms from 14 V to the buck's ~4.5 V cutoff at full
  load, 17 ms with the backlight and lamps shed: enough to stop SD writes
  and save the odometer, not enough for an `fsync`. A 470 µF / 35 V
  8 × 10.5 mm can (C48935389) across H2 and a ground pad would add about
  25 ms more; charge it through 10 Ω with an SS34 bypass, or use a
  slow-blow fuse, because its switch-on inrush is borderline for a 1 A
  fast fuse.

- **Test pads H1–H10** had a 2.54 mm through-hole header (C492400) on an
  SMD test-pad footprint in the BOM. Removed from the BOM.
- **U5 (TLE42754D) pin 3** is on no net, and the datasheet marks it not
  connected. The part is grounded through its tab (pad 6), so the tab has
  to be soldered.
- **SW2 is rated 2 A** (HX SS-12D10L3 datasheet: AC 125 V 2 A, contact
  resistance 100 mΩ new / 500 mΩ end of life, 5000 cycles, −25 to
  +70 °C). The audit's 0.5 A was a guess. Against about 0.45 A worst-case
  input that is ample. It has no DC rating and is not sealed: keep it out
  of the spray.
- **GPS RF trace** U4-11 to JP1 widened to 0.4 mm, about 47 Ω on this
  stackup.

- **U2 thermal vias.** Seven 0.3/0.6 mm vias added in U2's exposed pad
  (eight with the original), all on GND (PCB 2026-10-06 rev 4). In1 is
  solid under U2 and the Top and Bottom GND pours connect to them.

### 6.2 Open — check at bring-up

- **The neutral input pulls the bike's neutral wire up** (it works on the
  first board, on the bike). VIN → R6
  (2.2 kΩ) → D1 → the PC817 LED → `NEUTRAL_12V`, so with the switch open
  the wire sits near 12 V. If anything else on the bike shares that wire
  with a 5 V pull-up, it now sees 12 V. Measure the stock harness first.
  4.7 kΩ for R6 still saturates the opto.
- **USB-only power is marginal.** VIN is about 4.3–4.6 V after D5, at the
  LMR51430's 4.5 V minimum, and the backlight LDO is in dropout. On the
  first board the SD card did not mount on USB alone and did on 12 V.
- **The GPS never hears the P4** (first board, 2026-11-01). GPIO3 drives
  UART1 TX (IO-matrix dump), yet the PA1616D ignores every command, even at
  9600, so it stays at 1 Hz. Check U16-6 (GPIO3), the trace to U4-10 and
  U4-10's joint; then the level (3.3 V into a 2.8 V module). `bringup.md` §7.
- **GPS VBACKUP (U4-4) is tied to VCC**, so every power-up is a cold
  start. A supercap or coin cell through a diode buys hot starts.
- **Mechanical.** Four 3.0 mm corner holes, about 112 mm apart, nothing in
  the middle. Stake F1, C22, C23, L1; the push-push SD slot can eject on
  a dirt bike.

### 6.3 The tach pickup

The pickup is an **inductive wrap on the spark plug lead, 5–10 V at
most**, not the coil primary, so there is no high-voltage hazard on
`R34_1` or U11.

The risk ran the other way: **the input filter hid the pulse.** As
built, R34 (100 kΩ) with C19 (470 pF) is a 47 µs time constant, and a wrap
around the lead gives a spike of a few µs or less. A 1 µs, 10 V spike
charges C19 to about 0.2 V, far below the P4's ~2.5 V input threshold.

**First board (2026-10-28):** the wrap was fitted on the first ride and
there was no tach signal.

**Reworked (2026-11-07): R34 is 10 kΩ, C19 is 47 pF** (both 0805), a
~0.5 µs time constant: a 1 µs, 10 V spike reaches ~8–9 V and D2 clamps
it. R43 (1 MΩ) is a pull-down to GND and stays. At 10 V into 10 kΩ, D2
clamps about 0.65 mA. A signal generator into `TACH_SIG` made the dash
show rpm. **Not yet run on the bike, and the pickup is still not scoped**:

1. Scope the pickup on the running bike, at idle and at high rpm. Note
   peak voltage, pulse width and polarity (a negative-going pulse is
   clamped to GND and gives no edge: flip the wrap).
2. Check the reading against a known idle: the firmware assumes one spark
   per revolution; a wasted-spark-free ignition reads double.
3. Ringing gives several edges per spark, and 47 pF no longer filters it.
   `bsp::kTachRefractoryUs` (1.5 ms, a software window in the GPIO ISR,
   `engine_sense.cpp`) handles that; there is no PCNT or hardware glitch
   filter on the tach. At
   13 500 rpm, one spark per revolution is a 4.4 ms period.

### 6.4 Numbers from the audit

Figures as of the audit; the firmware has since moved on (render at
120 Hz, the pod polled at 200 Hz, the panel at a 25 MHz write clock:
`CLAUDE.md`).

| Quantity | Value |
|---|---|
| VCC load, idle / typical / worst | 165 / 380 / 1050 mA |
| Input at 14 V, idle / typical / worst | 85 / 175 / 400 mA (450 mA at 12 V) |
| PCA9955B full scale (REXT 1.91 kΩ) | 30 mA per channel |
| U5 dissipation, backlight 90 mA | 0.8–1.0 W |
| `VIN_SENSE` at 14 V | 2.54 V |
| Panel full frame, RGB565 at 10 MHz | 30.7 ms: two periods of the 60 Hz render loop |
| Panel partial (~15 % of screen) | about 4.7 ms, 28 % of a 60 Hz period |
| PCA9955B update at 500 Hz | 4.6 % of I2C at 1 MHz |
| Pod poll at 100 Hz | about 5 % of I2C at 100 kHz |
| GPS at 10 Hz, RMC+GGA+VTG+GSA | about 2.4 kB/s: 42 % of 57 600 baud, overruns 9600 |
| Board | 120.35 × 68.58 mm, 4 layers, 327 vias at 0.3/0.6 mm |
| Parts | 123: 115 top, 8 bottom (LED0–LED7), 3 through-hole |
| BOM, qty 1, without U4/U16 | about $32, of which the IMU is $19.64 |

### Panel chip-select during flashing (found 2026-11-05)

While esptool flashes, the P4 sits in its ROM loader for ~25 s with the
panel's CS (GPIO50) and WR (GPIO52) undriven, and the panel takes noise as
writes: the top-left of the screen garbles until the new image boots and
repaints. Harmless, but a pull-up on CS (10 k to 3V3) on the next board
would hold the panel deselected whenever the P4 is not driving it.
