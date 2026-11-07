Abstract interfaces only — no implementation, no ESP headers. They build on
the host, so a service can be exercised against a fake.

`status.hpp` `i_clock.hpp` `i_imu.hpp` `i_gnss.hpp` `i_gpio_expander.hpp`
`i_engine.hpp` `i_display.hpp` `i_storage.hpp` `i_shift_lamps.hpp`
`i_rails.hpp`, plus `hal.hpp` as an umbrella for main.

`i_engine.hpp` carries both halves of "engine data": `IEngine` is the ECU,
and `NullEngine` — always offline — is what ships, because the board has no
CAN transceiver (architecture §10.1). `IEngineSense` is the tach pickup, the
neutral switch and the wheel sensors, which are always available and owe
the ECU nothing.

The directory is `dash_hal` rather than `hal` because ESP-IDF already has a
component called `hal`, and a project component with the same name replaces
it (the build then dies in `espcoredump` with `hal/efuse_hal.h: No such
file`). The include prefix and the namespace are both still `hal`.
