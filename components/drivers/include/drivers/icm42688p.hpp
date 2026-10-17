// ICM-42688-P on SPI2.
//
// Six axes and no magnetometer — there is no magnetic sensing element in the
// package, so no amount of firmware gets a compass out of it (§8). Heading
// comes from GPS course, and the DIAG screen says so.
//
// The FIFO is not optional. This part runs at 200 Hz on a bus it shares with
// the SD card and the expander; one sample per interrupt drops data as soon
// as the logger writes a block.
#pragma once
#include "hal/i_imu.hpp"

namespace drivers {

class Icm42688p final : public hal::IImu {
public:
    hal::Status init() override;
    hal::Status read_fifo(Sample* out, size_t max, size_t& count) override;
    hal::Status set_gyro_bias(float gx_dps, float gy_dps, float gz_dps) override;

    // Whether the part answered its WHO_AM_I. A wrong answer here is
    // usually MISO, not the part.
    bool identified() const { return identified_; }

private:
    hal::Status write_reg(uint8_t reg, uint8_t value);
    hal::Status read_regs(uint8_t reg, uint8_t* out, size_t len);

    void*  dev_ = nullptr;         // spi_device_handle_t
    // read_regs()'s transfer buffers: a FIFO drain is up to 512 bytes plus
    // the address. Members, not locals, because they were 1 kB of the imu
    // task's 4 kB stack; only that task reads this part.
    static constexpr size_t kMaxRead = 512;
    uint8_t tx_[kMaxRead + 8] = {};
    uint8_t rx_[kMaxRead + 8] = {};
    float  bias_[3] = {0.0f, 0.0f, 0.0f};
    float  gyro_scale_ = 0.0f;     // LSB -> dps
    float  accel_scale_ = 0.0f;    // LSB -> g
    bool   identified_ = false;
    // The last sample's timestamp, so the stream stays strictly increasing
    // across reads whatever the poll timing does.
    uint32_t last_t_ms_ = 0;
    bool     have_last_ = false;
};

}  // namespace drivers
