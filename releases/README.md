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

