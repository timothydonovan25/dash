// The IMU: six numbers and a temperature.
//
// Axis convention is core/attitude's, and satisfying it is this driver's
// job: x forward, y right, z up, in the bike's frame. Upright at rest reads
// az = +1 g. Leaning right puts a positive number on ay. Get this wrong and
// the lean gauge is smooth, confident and backwards.
#pragma once
#include <cstddef>
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IImu {
public:
    virtual ~IImu() = default;

    struct Sample {
        float    gx_dps = 0.0f, gy_dps = 0.0f, gz_dps = 0.0f;
        float    ax_g = 0.0f, ay_g = 0.0f, az_g = 1.0f;
        float    temp_c = 0.0f;
        uint32_t t_ms = 0;
        // The part's own 16-bit timestamp and the packet's header byte, as
        // they came, for the detail log only: t_ms is worked out from the
        // P4's clock, and these let a ride measure how far off that is.
        uint16_t chip_ts = 0;
        uint8_t  fifo_hdr = 0;
    };

    virtual Status init() = 0;

    // Drain up to `max` samples. Reading one sample per interrupt drops data
    // on a part clocked at 200 Hz sharing SPI2 with the card and the
    // expander; the FIFO exists so this can be a burst.
    virtual Status read_fifo(Sample* out, size_t max, size_t& count) = 0;

    // Zero from a stationary bike at boot. Not the mount offset — that is
    // core/attitude's, and it lives in NVS.
    virtual Status set_gyro_bias(float gx_dps, float gy_dps, float gz_dps) = 0;
};

}  // namespace hal
