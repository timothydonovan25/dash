# ESP32-P4 demo build

A separate ESP-IDF project (own `sdkconfig.defaults`, own `main/`) that runs
the real dashboard UI, core logic and several production services on a
Guition **JC4880P443C_I_W** (ESP32-P4 + ESP32-C6, 4.3" 480×800 IPS MIPI-DSI
panel, GT911 capacitive touch, plastic shell). It was built while the bike
board was on order, and is still where UI, BLE, Wi-Fi, OTA and Doom are
tried first, away from the bike. It is not a step toward the bike board.

**Status: working on the physical board.** Panel, touch zones, the pod, the
microSD card, BLE (phone app and watch), the Wi-Fi page and OTA slots have
all run here. The bike data reads parked with no sensors (zeros and `--`).

## Build

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh
cd demo_p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py -C c6_cp set-target esp32c6 build     # the C6's firmware, once
idf.py -p /dev/ttyACM0 c6-flash              # into slave_fw, when c6_cp changes
```

The P4 is silicon **v1.3** (pre-production): `sdkconfig.defaults` keeps
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. An image for the bike (v3.2) does not
run here, nor the reverse. It uses the bike's `partitions.csv`, so the same
stale-`sdkconfig` guards as the bike's `main.cpp` apply. **An esptool or
USB-JTAG reset can wedge this board in a boot loop** (ROM SHA fault); only
unplugging it fixes that.

## What it reuses, and what is its own

Reused by source, not copied (`main/CMakeLists.txt` says why: the
`services` and `drivers` IDF components would drag in the bike's `bsp`):
`core`, `ui`, `dash_hal`, `doom`, `services::logic_tick` and the command
queue, and the production `ota`, `ble`, `c6_firmware`, `settings_store`,
`wifi` and `wifi_page` services, plus the `nvs_storage`, `clock`,
`chip_temp` and `sdcard` drivers (all but the mount). The point is to test
those, not copies.

Its own, under `main/`:

| File | Does |
|---|---|
| `p4_panel.{hpp,cpp}`, `p4_pins.hpp` | The display: a bare `esp_lcd_new_panel_dpi` panel, ST7701 init sent as raw DBI writes. The UI renders at 480×320 and is upscaled 1.5× (nearest neighbour, `P4Panel::blit()`) to 720×480, centred |
| `touch_buttons.{hpp,cpp}` | GT911 taps on five right-edge zones (A–E top to bottom) into the raw button mask `core::input` debounces |
| `pod_buttons.{hpp,cpp}` | The real handlebar pod (MCP23017, 0x20) on CN3 (1 GND, 2 3V3, 3 SCL = GPIO8, 4 SDA = GPIO7), sharing the touch bus and ORed into the mask. Optional; found within a second when plugged in |
| `sdcard_sdmmc.cpp` | The card mount: the Guition's microSD on 4-bit SDMMC slot 0 (CLK 43, CMD 44, D0–3 39–42), powered from LDO VO4. ESP-Hosted owns the one SDMMC controller for the C6, so this only adds slot 0 and never inits or deinits the host. Everything else is the bike's `drivers::SdCard` |
| `demo_render.{hpp,cpp}` | The render loop, simplified: this panel has no wire budget to protect. Its DSI panel scans out of PSRAM, so a flash write (an upload) makes it flicker; `CONFIG_SPIRAM_XIP_FROM_PSRAM` asserted at startup and was reverted (release 1.1) |
| `demo_bike_task.{hpp,cpp}` | Runs `logic_tick`; no fake ride, as the board has no bike sensors |
| `doom_program.{hpp,cpp}` | This board's `doom::Board` (features §31.1) |
| `serial_buttons.{hpp,cpp}` | The console: `btn`/`long`, `inputs`, `state`, `regs` (`docs/console.md`) |
| `ctrl_probe.{hpp,cpp}`, `state_request.hpp` | Bench probes behind the console |
| `main.cpp` | Wiring, as `main/wiring.cpp` does for the bike |

`c6_cp/` is Espressif's ESP-Hosted `slave` (network_adapter) example,
copied wholesale, configured for SDIO with BLE and Wi-Fi
(`CONFIG_ESP_HOSTED_CP_WIFI=y`). Both boards' `slave_fw` hold its image.

## Bring-up lessons

**Display.** Three bugs, in the order found:

1. **The DSI PHY has no power without an explicit LDO request**:
   `esp_ldo_acquire_channel` on channel 3 at 2500 mV (`kDsiPhyLdoChan`/
   `kDsiPhyLdoMv`) before MIPI-DSI does anything. Without it every DSI
   command hangs silently.
2. **`esp_lcd_st7701`'s LCD-ID read hangs** on this board's DSI reverse
   channel: the HAL busy-waits on the read FIFO with no timeout, and the
   task watchdog takes the app down. The vendor driver is no longer used.
3. **The panel has a real reset line (GPIO5)** that must be pulsed first.
   The recipe and pins (`kPanelReset=5`, `kTouchRst=22`, `kTouchInt=21`,
   `kPanelBacklight=23`) came from
   [elik745i/ESP32-2432S024C-Remote](https://github.com/elik745i/ESP32-2432S024C-Remote)'s
   `boards/jc4880p443c_i_w.json` and `src/jc4880p443_bringup_main.cpp`.
   `kRotateCw` was right on the first boot.

**Touch.** Two bugs, found by reading `esp_lcd_touch_gt911`'s source:

1. The GT911 address-select procedure only runs if
   `esp_lcd_touch_config_t.driver_data` names the address; without it the
   log says "I2C address initialization procedure skipped". Pass an
   `esp_lcd_touch_io_gt911_config_t` targeting **0x14** (0x5D NACKs here).
2. `x_max`/`y_max` must be the panel's **raw** 480×800, not the view size:
   `esp_lcd_touch.c` mirrors raw axes before `swap_xy`, and `480 - raw_y`
   underflowed to ~65500 for taps above raw_y = 480.

**Button zones.** Five zones, A–E top to bottom (matching
`sim/dash_sim.py`'s `BUTTONS`), across a 200 px strip at the right edge
(`kZoneWidth`), with four 1 px dividers drawn once at boot into the unused
margin. `kZoneWidth`/`kBandH` in `touch_buttons.cpp` and
`kMarginX`/`kBandH` in `p4_panel.cpp` must move together; they are
deliberately duplicated.

**ESP-Hosted.** Read the fetched `managed_components/` sources, not the
GitHub docs:

1. The registry's `espressif/esp_hosted` resolves to an older API than
   GitHub `main` documents (`esp_hosted_bt_controller_init()`/`_enable()`,
   not `esp_hosted_bt_host_stack_setup()`). Every radio dependency is now
   pinned exactly.
2. **`espressif/esp_wifi_remote` is load-bearing**: it defines
   `SLAVE_IDF_TARGET_ESP32C6`, and `CONFIG_ESP_HOSTED_ENABLED` depends on
   it. Without it the CP target and transport silently default to an
   ESP32-H2 over SPI.
3. This board's C6 shipped with ESP-Hosted 2.3.x, which cannot speak to a
   2.12 host. The fix needed no UART: the P4 pushes the C6's firmware over
   the SDIO link from `slave_fw` (`esp_hosted_ota.h`), which became the
   bike's `services/src/c6_firmware.cpp` and `core/c6`.
