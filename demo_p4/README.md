# ESP32-P4 demo build

A separate ESP-IDF project (own target, own `sdkconfig.defaults`, own
`main/`) that runs the real dashboard firmware's UI and core logic on a
generic ESP32-P4 + ESP32-C6 dev board — 4.3", 480x800 IPS capacitive touch,
camera, plastic shell — for demos, since the real CRF250F PCB is still at
fab. It exists so the UI can be shown off without real bike hardware; it is
not a step toward running on the real board, and it never becomes one.

It depends on `../components/core`, `../components/ui` and
`../components/dash_hal` unmodified, and reuses `services::logic_tick`,
`services::CommandQueue` and `sim::Bike` by source (see `main/CMakeLists.txt`
for why: pulling in the `services` IDF component itself would drag in
`bsp`/`drivers`, which are wired for the real board's S3 pin map and have no
business in a P4 build). `components/bsp/`, `components/drivers/`, `main/`
and the real `services/*` sources are untouched by anything here.

New code, all under `main/`:

- `p4_panel.{hpp,cpp}` / `p4_pins.hpp` — the display driver, `hal::IPanel`
  against an assumed ST7701 MIPI-DSI panel.
- `touch_buttons.{hpp,cpp}` — reads an assumed GT911 touch controller and
  turns taps on five on-screen zones into the same raw button mask
  `core::input::Decoder` already knows how to debounce.
- `demo_render.{hpp,cpp}` — the render loop, simplified from the real
  `render_service.cpp` because this panel has no SPI wire budget to protect.
- `demo_bike_task.{hpp,cpp}` — drives `sim::Bike` and `services::logic_tick`
  from an animated (not slider-driven) `Controls`, so the demo runs
  unattended.
- `main.cpp` — wiring: the same one-function, no-globals job
  `main/wiring.cpp` does for the real board, trimmed to what a screen-only
  demo needs.

The exact panel/touch controller ICs for this dropship board aren't
published anywhere — see `docs/bringup-p4-demo.md` for the bring-up
procedure and what to try if the picture doesn't come up right first try.

## Build

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
cd demo_p4
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
