# ESP32-P4 demo build

A separate ESP-IDF project (own target, own `sdkconfig.defaults`, own
`main/`) that runs the real dashboard firmware's UI and core logic on a
Guition JC4880P443C_I_W board (ESP32-P4 + ESP32-C6, 4.3", 480x800 IPS
capacitive touch, camera, plastic shell) for demos, since the real CRF250F
PCB is ordered and not here yet. It exists so the UI can be shown off without real bike
hardware; it is not a step toward running on the real board, and it never
becomes one.

**Status: fully working**, confirmed on real hardware — the dashboard UI
renders correctly (scaled 1.5x), all five on-screen button zones respond
to touch, the bike data reads parked with no sensors (zeros and `--`,
as the real dash shows with nothing connected), so the start-up gauge
sweep plays at every boot, and BLE (§11.1 Dash Media) is up and connected
to the phone app (`~/phone-app`): the C6 co-processor's stock firmware
gets upgraded automatically at boot over the existing SDIO link if it's
behind what this host expects (no UART adapter needed — see
`main/ota_slave.cpp`), NimBLE advertises as "CRF250F-Dash", and once
connected the speed screen's progress ring tracks the phone's now-playing
media in real time. Short C/D/E on that screen send previous/next/
play-pause back to the phone over the Ctrl characteristic — see
`docs/phone-media-app.md`'s "Control from the handlebars". DIAG's Radio
screen shows the link's own health: host/phone state, RSSI, the last
accepted tag, and whether the phone has subscribed to Ctrl. See
`docs/bringup-p4-demo.md` for the full story.

It depends on `../components/core`, `../components/ui` and
`../components/dash_hal` unmodified, and reuses `services::logic_tick` and
`services::CommandQueue` by source (see `main/CMakeLists.txt`
for why: pulling in the `services` IDF component itself would drag in
`bsp`/`drivers`, which are wired for the real board's S3 pin map and have no
business in a P4 build). `components/bsp/`, `components/drivers/`, `main/`
and the real `services/*` sources are untouched by anything here.

New code, all under `main/`:

- `p4_panel.{hpp,cpp}` / `p4_pins.hpp` — the display driver, `hal::IPanel`
  against the panel's bare MIPI-DSI/DPI interface (an ST7701, driven
  directly rather than through the generic vendor component — see
  `docs/bringup-p4-demo.md` for why).
- `touch_buttons.{hpp,cpp}` — reads a GT911 touch controller and turns taps
  on five right-edge zones (A-E, top to bottom) into the same raw button
  mask `core::input::Decoder` already knows how to debounce.
- `demo_render.{hpp,cpp}` — the render loop, simplified from the real
  `render_service.cpp` because this panel has no SPI wire budget to protect.
- `demo_bike_task.{hpp,cpp}` — runs `services::logic_tick` and hands BLE
  media in and Ctrl out. It no longer fakes a ride: the board has no bike
  sensors, so those slots are never written.
- `main.cpp` — wiring: the same one-function, no-globals job
  `main/wiring.cpp` does for the real board, trimmed to what a screen-only
  demo needs.
- `ble_media.{hpp,cpp}` — the §11.1 "Dash Media" NimBLE GATT server,
  running as an ESP-Hosted host with the C6 as its BT controller over SDIO.
- `ota_slave.{hpp,cpp}` — upgrades the C6's ESP-Hosted firmware over that
  same SDIO link at boot, if it's older than this host expects.

See `docs/bringup-p4-demo.md` for the full bring-up story.

## Build

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
cd demo_p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The C6's own firmware (`c6_cp/`, a separate ESP-IDF project targeting
`esp32c6`) has to be built and pushed into the `slave_fw` partition
separately — `idf.py flash` above only flashes the P4 side.
`main/CMakeLists.txt` is meant to fold that in automatically via
`esptool_py_flash_to_partition`, but that hook doesn't currently land the
image in `flasher_args.json` (see `docs/bringup-p4-demo.md`), so until
that's fixed, do it by hand once:

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py -C c6_cp -B c6_cp/build build
python3 -m esptool --chip esp32p4 -p /dev/ttyACM0 write-flash --force \
    0x610000 c6_cp/build/network_adapter.bin
```

After that, `ota_slave.cpp` keeps the C6 current on every boot — no need
to repeat this unless `c6_cp/`'s firmware changes.
