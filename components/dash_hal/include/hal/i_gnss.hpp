// The GPS receiver. Bytes only.
//
// There is no `read_position()` here on purpose. Every sentence is parsed by
// `core/nmea`, which builds and runs on the host, so a fix that reads 4230
// degrees north is a five-second test run rather than a ride with a laptop
// bungeed to the seat.
#pragma once
#include <cstddef>
#include "hal/status.hpp"

namespace hal {

class IGnss {
public:
    virtual ~IGnss() = default;
    virtual Status init() = 0;

    // Non-blocking. Ok with count == 0 means nothing has arrived yet, which
    // is the normal case at 50 Hz against a 10 Hz receiver.
    virtual Status read(char* out, size_t max, size_t& count) = 0;

    // Body only, without the '$' or the checksum — the driver adds those.
    // Used to set the fix rate at boot. Not required to work.
    virtual Status send_command(const char* nmea_body) = 0;
};

}  // namespace hal
