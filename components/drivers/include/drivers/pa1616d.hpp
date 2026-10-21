// PA1616D (MTK3333) on UART1. Bytes in, nothing else, once it is set up.
//
// Parsing lives in core/nmea, which builds on the host. This file is a UART
// and a ring buffer, and that is the whole point of the split. Setting the
// receiver up is the exception: init() listens for real sentences and for
// the receiver's PMTK001 answers (core::nmea::LineReader), because a byte
// arriving says nothing about whether the two ends agree on a baud rate.
#pragma once
#include "hal/i_gnss.hpp"

namespace drivers {

class Pa1616d final : public hal::IGnss {
public:
    hal::Status init() override;
    hal::Status read(char* out, size_t max, size_t& count, uint32_t wait_ms) override;
    hal::Status send_command(const char* nmea_body) override;
    bool full_rate() const override { return full_rate_; }

private:
    hal::Status set_baud(int baud);
    // A sentence with a good checksum within `ms`, at the current rate.
    bool hears_nmea(uint32_t ms);
    // Sends `body` and waits up to `ms` for its PMTK001. Ok only on flag 3.
    hal::Status command(const char* body, uint16_t cmd, uint32_t ms);

    bool installed_ = false;
    bool full_rate_ = false;
    int  baud_ = 0;
};

}  // namespace drivers
