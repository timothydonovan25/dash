The thirteen tasks. Each glues drivers to core logic and publishes into
state; each is a loop plus a health report. If one starts to look clever,
the clever part belongs in `core/` where it can be tested.

| Core 0 (real-time) | Core 1 (can be late) |
|---|---|
| `ctrl` 18, 500 Hz + every tach edge | `render` 6, 120 Hz |
| `imu` 15, 200 Hz | `logger` 2 |
| `gnss` 12 | `storage` 3 |
| `logic` 10, 50 Hz + on demand (`logic_tick.cpp`) | `keeper` 3, 20 Hz |
| `input` 9, 200 Hz | `power` 22, 1 kHz |
| | `ota` 2, `ble` 4, `wifi` 3 |

Core and priority are set where each `*_service.cpp` creates its task;
`docs/system-architecture.md` §5 has what each owns. Core 0 never gets a
radio or a filesystem.

- `ble` — the §11.1 Dash Media server, an iPhone's AMS/ANCS, and the watch's
  heart rate, over ESP-Hosted to the C6. NimBLE's callbacks only queue; `ble`
  is the one writer of `media`, `notify`, `heart` and `ble_diag`. It lowers
  ESP-Hosted's tasks (23, no affinity) to 8 by name, and `c6_firmware.cpp`
  keeps the C6's firmware current from `slave_fw`.
- `ota` — the stable/dev slots (§11.7), the only writer of the dev slot,
  every flash write parked-only, reading an install from the card or from
  `Deps::ota_stream` (a Wi-Fi upload). Decisions are `core::ota`'s.
- `wifi` + `wifi_page.cpp` — the on-demand hotspot and its one page
  (§11.7.1). It never writes flash; policy is `core::wifi`'s.
- `storage` — owns the card's mount, eject, and the track files
  (`track_store.cpp`, `TrackMailbox`); the seam for USB mass storage, which
  is not written and cannot be done by halves.
- `keeper` — health states, retry policy (`Deps::retry_requests`: the task
  that owns a device re-inits it), NVS saves while still
  (`settings_store.cpp`), backlight level, the console summary.
- `stack_watch.cpp`, `watchdog.cpp` — stack high-water marks; the task
  watchdog for `ctrl`, `imu`, `logic`, `render`.

The TPMS scan is not written: it waits on a sensor to capture.
