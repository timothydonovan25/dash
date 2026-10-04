# panel_test — the ILI9488 on a bare ESP32-S3 dev board

Step one of `docs/s3-parallel-respin.md` §2: before laying out a respin,
prove the panel runs on an 8-bit 8080 bus, that all eight data lines work,
and measure a full frame. It also runs the shipped board's 4-line SPI mode,
which is the first thing to try: if SPI does not light the panel, the fault
is power, reset or the backlight, not the parallel setup.

**Builds clean, never run.** Both interfaces compile against IDF v6.0.2
without warnings. Nothing has been flashed to a board.

Standalone on purpose. It uses nothing from `components/`, only
`esp_lcd_ili9488` at the firmware's version, so a failure here is the
panel or the wiring.

## Build and flash

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh
cd panel_test
idf.py set-target esp32s3
idf.py menuconfig        # "Panel test": interface, clock, pins
idf.py -p /dev/ttyACM0 flash monitor
```

The defaults are 8-bit 8080 at 10 MHz on the respin pin map. For SPI,
choose it under *Panel interface*; the SPI pins default to the shipped
board's.

## Wiring

The panel goes into a 40-pin, 0.5 mm FPC breakout. FPC pin numbers below
are from the panel's specification (HT035SHV007NS,
`3.5寸TN_规格书_ILI9488_40PIN.pdf`).

**FPC 10 and 11 swap roles between modes.** In 8080, FPC 10 is RS (D/C)
and FPC 11 is WR. In 4-wire SPI, FPC 10 is SCL and FPC 11 is D/CX. The
firmware keeps the same GPIOs in both modes, so the two wires to FPC 10
and 11 cross over when you change mode.

| Signal | GPIO | FPC pin, 8080 | FPC pin, SPI |
|---|---|---|---|
| WR / SCLK | 9 | **11** (WR) | **10** (SCL) |
| D/C | 7 | **10** (RS) | **11** (D/CX) |
| CS | 48 | 9 | 9 |
| D0–D7 | 10, 40, 38, 39, 18, 3, 45, 46 | 17–24 (DB0–DB7), in order | — (GND) |
| MOSI / MISO | 10 / 40 | 13 to GND, 14 open | 13 (SDI) / 14 (SDO) |
| RST | 4 | 15 | 15 |
| RD | — | 12 to **3V3** | 12 to 3V3 |
| IM2, IM1, IM0 | — | 40, 39, 38 = **0, 1, 1** | **1, 1, 1** |
| DB8–DB15 | — | 25–32 to GND | 25–32 to GND |
| FMARK | — | 8 open | 8 open |
| Touch XL/YU/XR/YD | — | 1–4 open | 1–4 open |
| VCC (IOVCC 2.8–3.3 V) | 3V3 | 6, 7 | 6, 7 |
| LEDA | 5V | 33 | 33 |
| LEDK1–3 | GND via 60 Ω each | 34–36 | 34–36 |
| GND | GND | 5, 16, 37 | 5, 16, 37 |

The backlight is three LED strings on one anode, Vf 3.2 V typical (3.5 V
maximum), specified at 90 mA total. 60 Ω from 5 V gives about 30 mA per
string, which is the specified current.

- **Every ground.** Ground every GND pin the datasheet names, and keep the
  data wires short and the same length. At 10 MHz a long ground return
  is enough to fail the toggle test.
- **IO45 is the flash-voltage strap.** Nothing on it may pull high at
  reset, or a 3.3 V-flash module will not boot. The panel's data pins are
  inputs with RD high, so they are safe. If the board will not boot with
  the panel attached, this is the first suspect.
- **The dev board's RGB LED sits on GPIO48** (DevKitC-1 v1.0) or GPIO38
  (v1.1). It will flicker with CS or D2. Harmless.
- Move any pin in menuconfig if your dev board does not break it out.

### Adafruit Feather ESP32-S3 Reverse TFT

The respin pin map does not fit: GPIO 40, 42 and 45 belong to the on-board
TFT, and 46 and 48 are not on the headers. Build with the Feather preset
instead:

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.feather_rtft" build
```

| Signal | Feather pin | GPIO | FPC pin, 8080 | FPC pin, SPI |
|---|---|---|---|---|
| D0–D5 | A0, A1, A2, A3, A4, A5 | 18, 17, 16, 15, 14, 8 | 17–22 | — |
| D6, D7 | D5, D6 | 5, 6 | 23, 24 | — |
| WR / SCLK | D9 | 9 | **11** | **10** |
| D/C | D10 | 10 | **10** | **11** |
| CS | D11 | 11 | 9 | 9 |
| RST | D12 | 12 | 15 | 15 |
| MOSI / MISO (SPI mode) | A0 / A1 | 18 / 17 | — | 13 / 14 |
| 3V3, GND | 3V, GND | — | 6, 7 / 5, 16, 37 | same |
| LEDA | USB (5 V) | — | 33 | 33 |

The other FPC pins follow the table above: RD to 3V3, the IM straps, and
DB8–DB15 to GND.

It stays off the SPI header, because the on-board TFT listens on 35–37,
and off SDA/SCL, which have pull-ups. None of these pins is a strapping pin, so
the IO45 warning does not apply.

Flashing an ESP-IDF image replaces the Feather's UF2 bootloader. Adafruit's
factory-reset image for this board puts it back. If `idf.py flash` cannot
reset the board by itself, hold **D0 (BOOT)**, tap **Reset**, release
D0, and flash again.

## What you should see

The serial log announces each picture. Each is held for 3 s, and the
sequence repeats.

| Test | Right | Wrong, and what it means |
|---|---|---|
| **Solid** R, G, B, white, black | Flat fields | Dark: power, reset, backlight, IM straps, or not 8080 at all. Red and blue swapped: flip `rgb_ele_order`. Speckled or a wrong colour on i80: try *Flip pixel byte order*. Washed out or inverted: try the IPS init table. |
| **Ramps** red / green / blue | Smooth, dark left to bright right | A ramp that resets halfway, repeats, or jumps: a dead or swapped data line. See below. |
| **Corners** | White border on all four edges. Red top-left, green top-right, blue bottom-left | Mirrored or rotated: change `swap_xy` / `mirror` in `bring_up()`, then in `drivers/src/ili9488.cpp` to match. A missing edge: the panel is offset, so it needs `set_gap`. |
| **Toggle** | One flat grey-green | Speckle or shimmer: signal integrity. Shorter wires, better ground, lower clock. |
| **Timing** | Logged | — |

### Reading the ramps on the 8-bit bus

Each RGB565 pixel goes out as two writes, high byte first, so pixel bit
`k` travels on data line `D(k % 8)`. Each line carries one bit from each
byte:

| Line | High byte | Low byte | Most visible damage |
|---|---|---|---|
| D0 | green bit 3 | blue bit 0 | green ramp steps |
| D1 | green bit 4 | blue bit 1 | green ramp steps |
| D2 | **green MSB** | blue bit 2 | green ramp resets halfway |
| D3 | red bit 0 | blue bit 3 | blue ramp steps |
| D4 | red bit 1 | **blue MSB** | blue ramp resets halfway |
| D5 | red bit 2 | green bit 0 | red ramp steps |
| D6 | red bit 3 | green bit 1 | red ramp steps |
| D7 | **red MSB** | green bit 2 | red ramp resets halfway |

A stuck-high line brightens the same places that a stuck-low line darkens.
Two swapped lines give two damaged ramps at once.

## The number that matters

The timing line reads like:

```
frame: 21.3 ms (46.9 fps) at 15 MHz; wire floor 20.5 ms
```

The floor is the wire alone: 307 kB at one byte per WR on i80, 460 kB at
one bit per clock on SPI. Measured close to the floor means the bus is the
limit, as expected. Raise the clock in menuconfig until the toggle test
speckles, then back off. **Record the highest clean clock and its frame
time in `docs/s3-parallel-respin.md`** — §6 decides from that number
whether dirty rects can go. The datasheet's write cycle (about 66 ns, so
roughly 15 MHz) is the in-spec ceiling, whatever the bench tolerates.
