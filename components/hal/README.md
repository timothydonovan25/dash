Abstract interfaces only — no implementation, no ESP headers.

Planned: i_imu.hpp, i_gnss.hpp, i_engine.hpp, i_gpio_expander.hpp,
i_storage.hpp, i_clock.hpp

i_engine.hpp matters first: it lets NullEngine ship now and a real ECU
driver arrive later without touching anything above it. See
docs/system-architecture.md section 10.
