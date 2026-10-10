# Firmware releases

Every dash image ever built, named `dash-<version>-<date>-<board>-<feature>.bin`
from what is inside it (see `firmware.txt` and `cmake/dash_version.cmake`).
`bike` images are for the real board, `demo` for the Guition dev board; the
dash refuses an upload built for the other one.

Upload one from the dash's Wi-Fi page (DIAG -> Update -> Wi-Fi), then
Boot dev once, then Promote if it is good.

Known bad:
- `dash-1.1-2026-10-09-demo-no-upload-flicker.bin` asserts at startup
  (CONFIG_SPIRAM_XIP_FROM_PSRAM) and falls back to stable. Kept as a record.


C6 co-processor images (`c6-<esp_hosted version>-<date>-<feature>.bin`):
- `c6-2.12.8-2026-10-10-full-features.bin` is built from `demo_p4/c6_cp`: the
  whole BLE 5 controller (extended and coded-PHY advertising, periodic sync,
  6 connections) and Wi-Fi AP + station with WPA3. It goes into the P4's
  `slave_fw` partition (`idf.py -p /dev/ttyACM0 c6-flash` from `demo_p4/`),
  and the dash pushes it from there to the C6. It reports the same 2.12.8 as
  the build already on the C6, so the dash's "older than 2.12" check will
  not push it by itself.
