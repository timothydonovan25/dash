# Bringing up the ESP32-P4 demo board

`demo_p4/` runs the real dashboard UI and core logic — unmodified — against
a simulated bike, on a Guition JC4880P443C_I_W board (ESP32-P4 + ESP32-C6,
4.3", 480x800 IPS capacitive touch, camera, plastic shell) for demos. See
`demo_p4/README.md` for what it is and isn't.

**Status: fully working, confirmed on the physical board.** Display,
touch, and the five on-screen button zones all work. Chip is a real
ESP32-P4 revision v1.3 (early/pre-production silicon; `sdkconfig.defaults`
sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`/`REV_MIN_100` accordingly).

## What it took: the display

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
   removed once the next fix replaced the vendor driver entirely).
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

`kRotateCw` was confirmed correct on the first working boot — the picture
came up right-side-up with the module physically rotated to landscape as
described in the plan behind this folder. The UI renders at its native
480x320 and is upscaled 1.5x (nearest-neighbor, in `P4Panel::blit()`) to
720x480, centered in the panel's 800x480 view — confirmed sharp on real
hardware.

## What it took: touch

Two real bugs, again found by reading the actual `esp_lcd_touch`/
`esp_lcd_touch_gt911` component sources rather than guessing further:

1. **The GT911 address-select procedure was never running at all.**
   `esp_lcd_touch_gt911.c` only pulses INT/RST to latch a specific I2C
   address if it's told which address to target via
   `esp_lcd_touch_config_t.driver_data` — without it, it logs "I2C address
   initialization procedure skipped - using default GT9xx setup" (which we
   saw verbatim) and leaves the chip however it powered up. Fixed by
   passing an `esp_lcd_touch_io_gt911_config_t` through `driver_data` in
   `touch_buttons.cpp`, targeting the backup address (0x14 — the primary
   0x5D NACKed on this board).
2. **`x_max`/`y_max` were set to the final (post-swap) view size, but
   `esp_lcd_touch.c` applies `mirror_x`/`mirror_y` to the *raw* axes before
   `swap_xy` exchanges them.** With `y_max=480` (the view height) applied
   to a raw y that natively goes up to 800, `480 - raw_y` underflowed to
   ~65500 for any tap above raw_y=480, and after the swap that garbage
   landed in the *x* output — confirmed by temporarily logging raw
   coordinates and seeing values like 65267 that are exactly `65536 -
   small_number`. Fixed by setting `x_max`/`y_max` to the panel's native
   raw resolution (480x800) instead of the view size.

## Button zones

Five zones, top-to-bottom A-E (matching `sim/dash_sim.py`'s `BUTTONS` list
and column layout), touch-sensitive across a 200px-wide strip
right-justified against the panel (`kZoneWidth` in `touch_buttons.cpp`) —
wide enough to hit reliably, unlike the picture's own 40px margin. Four
1px divider lines mark the zone boundaries, drawn once at boot
(`p4_panel.cpp`, right after the framebuffer is obtained) into the actual
unwritten margin — the part of that 200px strip that overlaps the picture
itself has no visible line, since `blit()` would paint over it on the next
frame anyway, but the touch zone still extends that far.

If the zone width, line position, or button order ever need to change,
`kZoneWidth`/`kBandH` in `touch_buttons.cpp` and `kMarginX`/`kBandH` in
`p4_panel.cpp` have to move together — they're deliberately duplicated
rather than shared across a header, matching how the rest of this file
already duplicated the view geometry.
