The tasks. Each glues drivers to core logic and publishes into state.

`ctrl` `imu` `gnss` `logic` `input` `render` `logger` `storage` `keeper`
`ble`, plus the one command queue. Cores and priorities are fixed in docs/system-architecture.md §5 and
repeated at the top of `services.hpp`.

Core 0 is a real-time island; do not put a radio or a filesystem on it.

`ble` runs the Dash Media GATT server (§11.1) over the module's C6, through
ESP-Hosted. NimBLE's callbacks only queue what happened; `ble` is the one
writer of `media` and `ble_diag`, and the line parsing is `core::media`,
host-tested. Which phone may stay is `core::ble`'s call (§11.3); `ble` only
acts on it, and takes CONFIG's Forget / Pairing mode from `logic` through
the `ble_request` slot. It also lowers ESP-Hosted's own tasks, which the component
creates at priority 23 with no core affinity, below everything on core 0.
The TPMS scan is not written: it waits on a sensor to capture.

`ota` owns the two app slots (§11.7): it works out at boot whether this
image is stable, dev or a promotion on probation, confirms a promotion once
it has run the dash, and installs from the card into the dev slot. Every
flash write is parked-only. The decisions are `core::ota`'s.

`storage_service` owns the card and holds the seam where USB mass storage
goes. MSC itself is not written, and it cannot be done by halves: the device
has to unmount before a host mounts, or two writers corrupt the volume.

Every service is a loop plus a health report. If one starts to look clever,
the clever part belongs in `core/` where it can be tested — that is where
the debounce, the tach maths, the attitude filter and the lap timer already
went.
