# Bringing up the ESP32-P4 demo board

`demo_p4/` runs the real dashboard UI and core logic — unmodified — against
a simulated bike, on a generic AliExpress ESP32-P4 + ESP32-C6 board (4.3",
480x800 IPS capacitive touch, camera, plastic shell) for demos. See
`demo_p4/README.md` for what it is and isn't; this is the procedure for
getting a picture up, the way `docs/bringup.md` is for the real board.

**Everything in `demo_p4/main/p4_pins.hpp`, `p4_panel.cpp` and
`touch_buttons.cpp` is a guess**, not a verified fact. This board has no
published schematic. It builds cleanly against ESP-IDF v6.0.2 (confirmed:
`idf.py set-target esp32p4 && idf.py build` produces a flashable
`dash_demo_p4.bin`), which only proves the API calls are well-formed — none
of the actual panel/touch behavior has run.

## 0. Before applying power

Same rule as the real board: don't guess at wiring with the board powered.
If you can get a multimeter on it, trace the module's DSI/touch header pins
against `p4_pins.hpp` before flashing so the placeholders can be corrected
up front rather than by trial and error.

## 1. Flash and watch the log

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
cd demo_p4
idf.py -p /dev/ttyACM0 flash monitor
```

Expect two health lines at boot: `panel: <status>` and `touch: <status>`
(see `main.cpp`). `NotPresent` or `IoError` on the panel means the DSI bus
came up but the ST7701 either isn't there or didn't answer its init
sequence — see step 2. A crash before either line prints means the DSI bus
config itself is wrong (lane count, PHY clock source) rather than anything
panel-specific.

## 2. If the screen stays dark or shows garbage

In rough order of likelihood, all in `p4_panel.cpp`:

- **Wrong controller entirely.** ST7701 is the common choice for this panel
  class, but not the only one. If nothing in step 1 changes after fixing
  everything below, the controller may not be an ST7701 at all — the panel
  FPC sometimes has the driver IC's part number printed on it.
- **Video timing.** `dpi_config.video_timing` (porches, sync widths,
  `dpi_clock_freq_mhz`) are placeholders. A torn or shifted image usually
  means the clock is off; a completely blank panel with a healthy `Ok` from
  `init()` usually means the porches are wrong enough that the panel never
  syncs.
- **Reset/backlight pins.** `p4pins::kPanelReset`/`kPanelBacklight` are
  guesses. A panel that never leaves its post-reset blank state, or a
  correct image with the backlight visibly off, points here first — it's
  the cheapest thing to check with a multimeter.
- **Lane count / bit rate.** `p4pins::kDsiDataLanes` and
  `lane_bit_rate_mbps` in `init()` affect whether the DSI PHY links at all;
  a bus that fails to initialize (`esp_lcd_new_dsi_bus` returning an error
  in the log) means these are wrong before anything panel-specific runs.

## 3. Once a picture is up: orientation

The picture will very likely be rotated, mirrored, or both on first light,
because `kRotateCw` in `p4_panel.cpp` is a guess about which way you
physically mounted the module. Flip it (`false` ↔ `true`) and rebuild if the
480x320 UI appears sideways or upside down. There's no way to get this
right without seeing it — don't spend time reasoning about it in the
abstract.

## 4. Touch

Once the picture is confirmed right-side-up, the five button zones
(`touch_buttons.cpp`) need their own check: tap each of the five strips in
the left margin and confirm the right screen/mode reacts. If taps land on
the wrong zone, or nothing happens at all:

- Wrong zone → try flipping `tp_cfg.flags.swap_xy` / `mirror_x` / `mirror_y`
  in `touch_buttons.cpp`. These need to match whatever `kRotateCw` ended up
  being in step 3, and there's no way to derive the right combination
  without a working picture to tap on first.
- Nothing at all → `esp_lcd_touch_new_i2c_gt911` failing in the log points
  at `p4pins::kTouchSda/kTouchScl` (wrong I2C pins) or the controller not
  being a GT911 at all. `kTouchRst`/`kTouchInt` being wrong can also leave
  the chip permanently in reset.

## 5. Once it's all working

Nothing left to tune functionally — the UI, lap timer, shift pattern and
attitude filter are the real firmware's own code, exercised by
`sim::Bike` exactly as `sim/dash_sim.py` exercises them on a laptop. The one
optional next step is the 1.5x upscale mentioned in the plan behind this
folder, entirely inside `p4_panel.cpp::blit()`, if the margined 480x320
image looks too small in the case.
