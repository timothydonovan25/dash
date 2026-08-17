#include "drivers/icm42688p.hpp"
#include "core/attitude/attitude.hpp"
#include "bsp/pins.hpp"

#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace drivers {
namespace {

// Bank 0 registers.
constexpr uint8_t kDeviceConfig = 0x11;
constexpr uint8_t kFifoConfig   = 0x16;
constexpr uint8_t kIntfConfig0  = 0x4C;
constexpr uint8_t kPwrMgmt0     = 0x4E;
constexpr uint8_t kGyroConfig0  = 0x4F;
constexpr uint8_t kAccelConfig0 = 0x50;
constexpr uint8_t kFifoConfig1  = 0x5F;
constexpr uint8_t kFifoCountH   = 0x2E;
constexpr uint8_t kFifoData     = 0x30;
constexpr uint8_t kIntConfig    = 0x14;
constexpr uint8_t kIntConfig1   = 0x64;
constexpr uint8_t kIntSource0   = 0x65;
constexpr uint8_t kWhoAmI       = 0x75;
constexpr uint8_t kWhoAmIValue  = 0x47;

constexpr uint8_t kReadBit = 0x80;

// The configured ODR, as a period. If kGyroConfig0 / kAccelConfig0 change
// away from 200 Hz, this has to move with them — the two are set together
// below and there is nothing that will notice if they drift apart.
constexpr uint32_t kSamplePeriodMs = 5;

// One packet of FIFO format 16-byte "packet 3": header, accel xyz, gyro xyz,
// temperature, timestamp.
constexpr size_t kPacketBytes = 16;

hal::Status from_esp(int err) {
    if (err == ESP_OK) return hal::Status::Ok;
    if (err == ESP_ERR_TIMEOUT) return hal::Status::Timeout;
    return hal::Status::IoError;
}

int16_t be16(const uint8_t* p) {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

}  // namespace

hal::Status Icm42688p::write_reg(uint8_t reg, uint8_t value) {
    if (!dev_) return hal::Status::NotPresent;
    uint8_t tx[2] = {static_cast<uint8_t>(reg & 0x7F), value};
    spi_transaction_t t = {};
    t.length = 16;
    t.tx_buffer = tx;
    return from_esp(spi_device_polling_transmit(
        static_cast<spi_device_handle_t>(dev_), &t));
}

hal::Status Icm42688p::read_regs(uint8_t reg, uint8_t* out, size_t len) {
    if (!dev_) return hal::Status::NotPresent;
    if (len == 0 || len > 512) return hal::Status::Unsupported;

    uint8_t tx[520] = {};
    uint8_t rx[520] = {};
    tx[0] = static_cast<uint8_t>(reg | kReadBit);

    spi_transaction_t t = {};
    t.length = 8 * (len + 1);
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    const hal::Status s = from_esp(spi_device_transmit(
        static_cast<spi_device_handle_t>(dev_), &t));
    if (hal::ok(s)) {
        for (size_t i = 0; i < len; ++i) out[i] = rx[i + 1];
    }
    return s;
}

hal::Status Icm42688p::init() {
    if (!dev_) {
        spi_device_interface_config_t cfg = {};
        cfg.clock_speed_hz = bsp::kSpiHzImu;
        cfg.mode = 3;                       // CPOL=1, CPHA=1
        cfg.spics_io_num = bsp::kCsImu;
        cfg.queue_size = 4;
        spi_device_handle_t h = nullptr;
        const esp_err_t err = spi_bus_add_device(SPI2_HOST, &cfg, &h);
        if (err != ESP_OK) return from_esp(err);
        dev_ = h;
    }

    hal::Status s = write_reg(kDeviceConfig, 0x01);      // soft reset
    if (!hal::ok(s)) return s;
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t who = 0;
    s = read_regs(kWhoAmI, &who, 1);
    if (!hal::ok(s)) return s;
    identified_ = (who == kWhoAmIValue);
    if (!identified_) {
        // Answered, but not as this part. On a fresh board this is almost
        // always MISO shorted or swapped rather than a wrong chip.
        return hal::Status::BadReply;
    }

    // ±2000 dps and ±16 g at 200 Hz. A dirt bike lands harder than ±8 g and
    // a tank-slapper spins faster than 1000 dps; clipping either would show
    // up as a lean angle that quietly stops tracking.
    s = write_reg(kGyroConfig0, 0x07);      // 2000 dps, 200 Hz ODR
    if (!hal::ok(s)) return s;
    s = write_reg(kAccelConfig0, 0x07);     // 16 g, 200 Hz ODR
    if (!hal::ok(s)) return s;

    gyro_scale_ = 2000.0f / 32768.0f;
    accel_scale_ = 16.0f / 32768.0f;

    s = write_reg(kIntfConfig0, 0x30);      // FIFO count and data big-endian
    if (!hal::ok(s)) return s;
    s = write_reg(kFifoConfig, 0x40);       // stream-to-FIFO
    if (!hal::ok(s)) return s;
    s = write_reg(kFifoConfig1, 0x07);      // accel + gyro + temp in the FIFO
    if (!hal::ok(s)) return s;

    // Data-ready on INT1, so the imu task is woken by the part rather than
    // guessing at 5 ms. INT_ASYNC_RESET has to be cleared from its reset
    // value for the pin to behave — the datasheet says so in one line and
    // it is the usual reason a working configuration produces no edges.
    //
    // BRINGUP: these three values are read off the register map and have
    // never seen a real part. A wrong one costs nothing: the service waits
    // on the notification with a timeout and falls back to polling, which
    // is exactly what it did before, and the DIAG system screen counts how
    // often that happens.
    s = write_reg(kIntConfig1, 0x00);       // clear INT_ASYNC_RESET (bit 4)
    if (!hal::ok(s)) return s;
    s = write_reg(kIntConfig, 0x03);        // INT1 push-pull, active high, pulsed
    if (!hal::ok(s)) return s;
    s = write_reg(kIntSource0, 0x08);       // UI_DRDY_INT1_EN
    if (!hal::ok(s)) return s;

    // Gyro and accel both in low-noise mode. Written last: the datasheet
    // wants configuration set before the sensors are turned on.
    s = write_reg(kPwrMgmt0, 0x0F);
    vTaskDelay(pdMS_TO_TICKS(50));          // gyro start-up
    return s;
}

hal::Status Icm42688p::read_fifo(Sample* out, size_t max, size_t& count) {
    count = 0;
    if (!dev_ || !identified_) return hal::Status::NotPresent;

    uint8_t hdr[2] = {};
    hal::Status s = read_regs(kFifoCountH, hdr, 2);
    if (!hal::ok(s)) return s;

    size_t packets = ((static_cast<size_t>(hdr[0]) << 8) | hdr[1]) / kPacketBytes;
    if (packets == 0) return hal::Status::Ok;
    if (packets > max) packets = max;

    uint8_t buf[kPacketBytes * 16];
    const size_t batch = packets > 16 ? 16 : packets;
    s = read_regs(kFifoData, buf, batch * kPacketBytes);
    if (!hal::ok(s)) return s;

    // Back-dated, not stamped with the read time. Every sample in a batch
    // carrying one timestamp gives core/attitude a run of dt == 0, and it
    // drops those — so the whole 200 Hz stream collapsed to the poll rate
    // and aliased the vibration in between. See core::attitude::sample_time_ms,
    // which is where the arithmetic lives so it can be host-tested.
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < batch; ++i) {
        const uint8_t* p = buf + i * kPacketBytes;
        if ((p[0] & 0x80) != 0) continue;         // empty-FIFO header

        Sample sm;
        // Chip axes to bike axes: x forward, y right, z up, which is what
        // core/attitude documents and depends on. BRINGUP: this mapping is
        // taken from the board outline and has never been checked against a
        // real part. Roll the bike to the right on the stand and confirm the
        // lean number goes positive before trusting anything downstream.
        const float ax = be16(p + 1) * accel_scale_;
        const float ay = be16(p + 3) * accel_scale_;
        const float az = be16(p + 5) * accel_scale_;
        const float gx = be16(p + 7) * gyro_scale_;
        const float gy = be16(p + 9) * gyro_scale_;
        const float gz = be16(p + 11) * gyro_scale_;

        sm.ax_g = ax;
        sm.ay_g = ay;
        sm.az_g = az;
        sm.gx_dps = gx - bias_[0];
        sm.gy_dps = gy - bias_[1];
        sm.gz_dps = gz - bias_[2];
        sm.temp_c = static_cast<int8_t>(p[13]) / 2.07f + 25.0f;
        sm.t_ms = core::attitude::sample_time_ms(now_ms, i, batch,
                                                 kSamplePeriodMs);
        out[count++] = sm;
    }
    return hal::Status::Ok;
}

hal::Status Icm42688p::set_gyro_bias(float gx, float gy, float gz) {
    bias_[0] = gx;
    bias_[1] = gy;
    bias_[2] = gz;
    return hal::Status::Ok;
}

}  // namespace drivers
