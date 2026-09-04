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

## BLE, over the C6

The P4 has no radio of its own; NimBLE runs on the P4 as the ESP-Hosted
"host" and the actual BT controller runs on the C6, reached over a 4-bit
SDIO link (`ble_media.cpp`, `demo_p4/c6_cp/`, wired last in `main.cpp`
since it's the one piece that talks to another chip). Two real surprises,
both found by reading the locally-fetched `managed_components/` sources
directly rather than trusting the package's remote docs, which describe a
newer API than the registry actually resolves:

1. **The registry's `espressif/esp_hosted: "*"` resolves to an API older
   than the GitHub `main` branch documents.** The docs describe
   `esp_hosted_bt_host_stack_setup()`; the resolved package (reporting
   itself as "2.12.8" at CP-build time) only has
   `esp_hosted_bt_controller_init()`/`_enable()`, found by grepping the
   fetched `managed_components/espressif__esp_hosted/examples/
   host_nimble_bleprph_host_only_vhci/` example rather than the README.
2. **`espressif/esp_wifi_remote` is a load-bearing dependency despite the
   name** — it's where `esp_hosted`'s `SLAVE_IDF_TARGET_ESP32C6` Kconfig
   symbol is defined, and without it the CP target and transport silently
   default to an ESP32-H2/SPI combination that doesn't exist on this
   board, with no error, just wrong defaults. `CONFIG_ESP_HOSTED_ENABLED`
   itself is gated the same way (`depends on ESP_WIFI_REMOTE_...`) and
   must also be set explicitly.

`demo_p4/c6_cp/` is Espressif's own `slave`/`network_adapter` example
(copied wholesale from the managed component, not hand-written), built
with `CONFIG_ESP_HOSTED_CP_WIFI=n` since this demo has no use for Wi-Fi —
just the BT controller half.

**Both sides build and the P4 side is flashed and boots.** The boot log
shows the SDIO transport coming up and identifying the C6 correctly, but
`esp_hosted_bt_controller_init()` fails:

```
I transport: Identified slave [esp32c6]
I transport: capabilities: 0xd
I transport: 	 * WLAN / HCI over SDIO / BLE only
W transport: Version mismatch: Host [2.12.0] > Co-proc [2.3.0] ==> Upgrade co-proc to avoid RPC timeouts
I ble_media: C6 coprocessor firmware: 2.3.2
E ble_media: esp_hosted_bt_controller_init failed
```

The C6 on this board still has whatever firmware it shipped with (2.3.x,
does not speak the same ESP-Hosted RPC version as our 2.12.x host build)
— it has never been reflashed with `demo_p4/c6_cp/`'s output. This board
exposes only one USB-serial port (`/dev/ttyACM0`, the P4's), so the C6
can't be reflashed over USB the way the P4 is — it needs a USB-TTL adapter
wired directly to its UART0/boot/reset pins on the board's Expand-IO
header (JP1), with the P4 held in reset (or its own reset line to the C6,
GPIO54, disabled) so it doesn't reset the C6 mid-flash. That's the one
remaining step to a working BLE link — it's a physical wiring job, not a
software one.
