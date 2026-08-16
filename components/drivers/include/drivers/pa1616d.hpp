// PA1616D (MTK3333) on UART1. Bytes in, nothing else.
//
// Parsing lives in core/nmea, which builds on the host. This file is a UART
// and a ring buffer, and that is the whole point of the split.
#pragma once
#include "hal/i_gnss.hpp"

namespace drivers {

class Pa1616d final : public hal::IGnss {
public:
    hal::Status init() override;
    hal::Status read(char* out, size_t max, size_t& count) override;
    hal::Status send_command(const char* nmea_body) override;

private:
    hal::Status set_baud(int baud);

    bool installed_ = false;
    int  baud_ = 0;
};

}  // namespace drivers
