// The GPS receiver. Bytes only.
//
// There is no `read_position()` here on purpose. Every sentence is parsed by
// `core/nmea`, which builds and runs on the host, so a fix that reads 4230
// degrees north is a five-second test run rather than a ride with a laptop
// bungeed to the seat.
#pragma once
#include <cstddef>
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IGnss {
public:
    virtual ~IGnss() = default;
    virtual Status init() = 0;

    // Waits up to `wait_ms` for the first byte, then returns it with
    // whatever else has arrived, without waiting for `max`. Ok with
    // count == 0 means nothing came in that time, which is not an error.
    virtual Status read(char* out, size_t max, size_t& count, uint32_t wait_ms) = 0;

    // init() got the receiver to 10 Hz. False after an init that left it at
    // 1 Hz (9600 baud and it would not move, or 10 Hz refused): it works,
    // and `gnss` reports it limited, which the health table calls Degraded.
    virtual bool full_rate() const = 0;

    // Body only, without the '$' or the checksum — the driver adds those.
    // Used to set the fix rate at boot. Not required to work.
    virtual Status send_command(const char* nmea_body) = 0;
};

}  // namespace hal
