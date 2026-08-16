The tasks. Each glues drivers to core logic and publishes into state.

`ctrl` `imu` `gnss` `logic` `input` `render` `logger` `storage` `keeper`,
plus the one command queue. Cores and priorities are fixed in docs/system-architecture.md §5 and
repeated at the top of `services.hpp`.

Core 0 is a real-time island; do not put a radio or a filesystem on it.

`ble` is not built. It is step 9 of the build order, deliberately after the
real-time path works, so that if the shift-light timing changes you know the
radio caused it.

`storage_service` owns the card and holds the seam where USB mass storage
goes. MSC itself is not written, and it cannot be done by halves: the device
has to unmount before a host mounts, or two writers corrupt the volume.

Every service is a loop plus a health report. If one starts to look clever,
the clever part belongs in `core/` where it can be tested — that is where
the debounce, the tach maths, the attitude filter and the lap timer already
went.
