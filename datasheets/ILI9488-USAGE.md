# ILI9488 / HT035SHV007NS panel — usage

3.5" 320 × 480 **IPS** TFT with an Ilitek ILI9488, 40-pin FPC, used
landscape 480 × 320. `ILI9488.pdf` is the controller datasheet; the panel's
own spec sheet was a TN edition (`3.5寸TN_规格书_ILI9488_40PIN.pdf`, not
stored here) and its pin functions are transcribed in pin-map §4. Driver:
`components/drivers/src/ili9488.cpp`.

## Bus
- **8-bit 8080 parallel (IM[2:0] = 011), RGB565, two bytes per pixel. Not
  SPI.** `esp_lcd` i80 IO handle + the `esp_lcd_ili9488` component; do not
  hand-write the command set.
- D0..D7 = GPIO33 down to 26 (reversed; i80 takes any GPIO per bit).
  RST 53, WR 52 (via R30 22 Ω), DC 51, CS 50, TE 49 (FMARK).
- **FPC arrives flipped** into a top-contact connector: panel pin n lands
  on pad 41 − n.
- Write clock **25 MHz** (`kTftWrHz`), above the datasheet's ~15 MHz (66 ns
  cycle) but clean to 30 MHz on the first board. First thing to lower if it
  ever speckles hot.
- Full frame ≈ 12 ms; `render` sends only changed regions via `ui::Shadow`.

## Init
- **IPS init**: `esp_lcd_new_panel_ili9488_ips`. The default (TN) sequence
  gave a negative image / crawling speckle at every clock.
- `swap_xy` + mirror on both axes for the HT035 as fitted.
- Backlight is not the controller's: LEDK1–3 go to the PCA9955B, LEDA to U5.

## Panel probe
At boot the driver tells the HT035 from the UEED035HV by which WR/RS wiring
makes TE pulse after TEON (`UEED035HV-USAGE.md`).

## Trap
While esptool flashes (~25 s in the ROM loader) CS and WR are undriven and
the panel takes noise as writes: the top-left garbles until the new image
repaints. Harmless; a 10 k CS pull-up on the next board would stop it
(pin-map "Panel chip-select during flashing").
