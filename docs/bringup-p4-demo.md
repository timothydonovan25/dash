# Bringing up the ESP32-P4 demo board

`demo_p4/` runs the real dashboard UI and core logic — unmodified — against
a simulated bike, on a Guition JC4880P443C_I_W board (ESP32-P4 + ESP32-C6,
4.3", 480x800 IPS capacitive touch, camera, plastic shell) for demos. See
`demo_p4/README.md` for what it is and isn't.

**Status: the display works.** Panel init succeeds, and the dashboard UI —
scaled 1.5x — renders correctly on the real screen, confirmed against the
physical board. Touch does not work yet (§2 below). Chip is a real ESP32-P4
revision v1.3 (early/pre-production silicon; `sdkconfig.defaults` sets
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`/`REV_MIN_100` accordingly).

## What it took

Three real bugs, in the order they were found and fixed:

1. **The DSI PHY has no power without an explicit LDO request.** Every
   ESP32-P4 needs `esp_ldo_acquire_channel` on channel 3 at 2500mV
   (`kDsiPhyLdoChan`/`kDsiPhyLdoMv` in `p4_panel.cpp`) before the MIPI-DSI
   peripheral does anything — this is fixed silicon wiring, not a per-board
   guess, and skipping it means every DSI command silently hangs forever
   with no error, because the bus "exists" in software with nothing
   happening on the wire.
2. **The `esp_lcd_st7701` vendor component's own LCD-ID read hangs on this
   board's DSI reverse channel.** `esp_lcd_panel_io_rx_param`'s underlying
   HAL busy-waits on the read FIFO with no timeout, so an unanswered read
   takes the whole app down via the task watchdog. Worked around initially
   by forking the component to skip that one read (kept in git history;
   removed once §3 replaced the vendor driver entirely).
3. **The panel has a real hardware reset line (GPIO5) that must be pulsed
   before it responds to anything**, contrary to an early assumption (based
   on Espressif's *different* reference board's BSP header) that this
   panel had no reset pin. This was the actual fix: found via
   [elik745i/ESP32-2432S024C-Remote](https://github.com/elik745i/ESP32-2432S024C-Remote)'s
   `boards/jc4880p443c_i_w.json` + `src/jc4880p443_bringup_main.cpp`, a
   bring-up sketch written for this exact board variant. `p4_panel.cpp` now
   follows that recipe directly: a bare `esp_lcd_new_panel_dpi` panel (not
   the vendor ST7701 wrapper), a manual reset pulse, and that sketch's own
   confirmed init command table sent as raw DBI writes. Confirmed pins from
   the same source: `kPanelReset=5`, `kTouchRst=22`, `kTouchInt=21`,
   `kPanelBacklight=23` (the last also matches Espressif's
   `esp32_p4_function_ev_board` BSP header).

## Orientation and scale

`kRotateCw` in `p4_panel.cpp` was confirmed correct on the first working
boot — the picture came up right-side-up with the module physically
rotated to landscape as described in the plan behind this folder. If you
ever change how the module is mounted, that's the flag to flip.

The UI renders at its native 480x320 and is upscaled 1.5x (nearest-neighbor,
in `P4Panel::blit()`) to 720x480, centered in the panel's 800x480 view with
40px margins left/right — confirmed working and sharp on real hardware.

## Open: touch

The five touch-button zones don't work yet. Boot log shows:

```
GT911: I2C address initialization procedure skipped - using default GT9xx setup
E lcd_panel.io.i2c: panel_io_i2c_rx_buffer(145): i2c transaction failed
E GT911: touch_gt911_read_cfg(419): GT911 read error!
```

The touch chip never answers on I2C at all (not a wrong-zone mapping
problem — `esp_lcd_touch_new_i2c_gt911` itself fails). `kTouchSda=7`,
`kTouchScl=8` are confirmed against Espressif's own BSP header, and
`kTouchRst=22`/`kTouchInt=21` against the jc4880p443c_i_w bring-up sketch,
so the pins are the same class of "confirmed against a real matching
reference" as the pins that fixed the display — but evidently something is
still wrong. Candidates, cheapest first:

- **I2C address.** GT911 uses 0x5D or 0x14 depending on the level of its
  INT pin at power-on (see `ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS[_BACKUP]` in
  the `esp_lcd_touch_gt911` component). `touch_buttons.cpp` uses the
  macro's default; try the other address if this board's power-on strap
  differs.
- **Reset pulse.** `esp_lcd_touch_new_i2c_gt911` pulses `tp_cfg.rst_gpio_num`
  itself using `tp_cfg.levels.reset` — worth confirming that polarity
  against the jc4880p443_bringup_main.cpp's `pulseTouchReset()` (pulls low,
  waits, releases high) rather than trusting the driver's default.
- **Shared I2C bus contention or timing** — `i2c_master_bus_config_t` in
  `touch_buttons.cpp` uses default glitch filtering and no explicit clock
  stretching allowance; a NACK before any clock stretch completes would
  look exactly like this.

## Verification once touch works

Tap each of the five strips in the left margin and confirm the right
screen/mode reacts, the same way pressing A-E does in `sim/dash_sim.py`.
