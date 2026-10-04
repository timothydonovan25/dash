# P4 board pin map — Guition JC-ESP32P4-M3-C6 (U16)

**Status.** A proposed assignment for the P4 respin netlist, not yet
drawn. Nothing here has run. The module is a Guition JC-ESP32P4-M3-C6:
ESP32-P4NRW32 (32 MB in-package PSRAM, 16 MB flash) plus an
ESP32-C6-MINI-1U-N4 on SDIO, 27 × 27 mm, 80 castellated pins.

The panel runs on an **8-bit 8080 parallel bus**, not SPI. The panel's
datasheet (HT035SHV007NS, ILI9488, 40-pin FPC) lists 8080 8/16-bit and
3/4-wire SPI, and brings DB0–DB15 out on FPC 17–32. FPC pin functions in
§4 are taken from it. It has not run in parallel yet: `panel_test/` on
the breakout is the proof (`s3-parallel-respin.md` §2).

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
(`esp_lcd_new_i80_bus()`, RGB565, see `s3-parallel-respin.md` §6). The
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
| 45 | GPIO26 | `TFT_D0` | i80 data |
| 46 | GPIO27 | `TFT_D1` | |
| 47 | GPIO28 | `TFT_D2` | |
| 48 | GPIO29 | `TFT_D3` | |
| 49 | GPIO30 | `TFT_D4` | |
| 50 | GPIO31 | `TFT_D5` | |
| 51 | GPIO32 | `TFT_D6` | |
| 52 | GPIO33 | `TFT_D7` | |
| 53 | GPIO34 | `LED_SDA` | PCA9955B, own I2C bus at 1 MHz (§2.2). Strap pin, see §2.1. |
| 54 | GPIO35 | `FLASH` | BOOT button to GND + 10 kΩ pull-up. **Moved from pin 76.** |
| 55 | GPIO36 | 10 kΩ pull-up | required for download mode |
| 56 | GPIO37 | test pad `DEBUG_TX` | UART0 |
| 57 | GPIO38 | test pad `DEBUG_RX` | UART0 |
| 58 | — | — | `ESP_LDO_VO4`: leave unconnected |
| 59–68 | GPIO39–48 | — | leave unconnected (§1) |
| 69 | GPIO49 | `TFT_WR` | i80 write strobe, **FPC 11** |
| 70 | GPIO50 | `DC/RS` | **FPC 10** |
| 71 | GPIO51 | `TFT_CS` | keep R41's 10 kΩ pull-up: holds CS off while the pin floats at boot |
| 72 | GPIO52 | `TFT_RST` | keep R3's pull-up. Was on the MCP23S17 on the S3 board. Now a plain GPIO, which removes the "expander before panel" trap from `bringup.md`. |
| 73 | GPIO53 | `TFT_TE` | panel FMARK, **FPC 8**: tearing-effect output for tear-free updates. Optional. Was `TFT_BL`; the backlight is on the PCA9955B (§2.2). |
| 74 | GPIO54 | C6 reset | 10 kΩ pull-up + 1 µF to GND, as the dev board's R81/C64 |
| 75 | CHIP_PU | `ESP_EN` | R27, C30, SW3 already on the net |
| 76 | GPIO0 | `LED_SCL` | PCA9955B. `FLASH` was here; GPIO0 is not a boot pin. |
| 77, 78 | — | `VCC` | 3.3 V, already wired |

**No MCP23S17.** Everything it did has a home: the shift-lamp gates went
to the PCA9955B, `TFT_RST` to GPIO52, and `NEUTRAL` and
`SD_CARD_DETECT` to GPIO12 and GPIO1. The shared SPI bus now carries only
the IMU and the SD card.

**Gone:** `PPS`, `TFT_MISO`, and `TFT_SCLK`/`TFT_MOSI` as SPI nets. RD is
tied high, so nothing reads the panel back. `SHIFT_POWER_PWM_3V3`,
`SHIFT_CTL`, `SHIFT0`–`7`, `SHIFT_POWER0`–`7` and `TFT_BL` go with the FET
circuits (§2.2). `MCP_CS` goes with U5.

### 2.1 Spares

None outright. **GPIO53** (pin 73) is free unless `TFT_TE` is wired. The
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
The driver reports open and shorted LEDs as health; the firmware decides
what to do (rule 5).

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
power-source test `dash-v1-netlist-review.md` §3 asked for.

---

## 4. Netlist changes this implies

1. `FLASH`: U16-76 → **U16-54**. Add 10 kΩ to VCC on pin 54 and on pin 55.
2. `ESP_EN` → **U16-75**.
3. Pin 74: 10 kΩ to VCC, 1 µF to GND. Pin 18: 10 kΩ to VCC.
4. Test pads on pins 16, 17, 56, 57. Pins 42/43 go to the USB-C instead.
5. New net `VIN_SENSE` with the divider and clamp in §3.
6. Delete `PPS` (U4-13 left open) and `TFT_MISO`.
7. **FPC1, from the panel datasheet** (pin: function → net):

   | FPC | Function | Net | Change from today |
   |---|---|---|---|
   | 1–4 | resistive touch XL/YU/XR/YD | open | none |
   | 5, 16, 37 | GND | GND | none |
   | 6, 7 | IOVCC 2.8–3.3 V | VCC | none |
   | 8 | FMARK (tearing effect) | `TFT_TE` | new; open if not wired |
   | 9 | CS | `TFT_CS` | none, keep R41 |
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

- **−20 to +70 °C.** `p4-platform-selection.md` §7 expects a dark dash in
  sun to reach 70–80 °C before the backlight. That is at or above this
  module's rating. The panel is rated the same: −20 to +70 °C operating,
  −30 to +80 °C storage.
- **Chip revision.** The P4 demo board was revision v1.3, which needs
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Read it with esptool on arrival.
- **Antenna.** The C6 is the MINI-1U, with an IPEX connector. It needs an
  external antenna routed out of the enclosure.
