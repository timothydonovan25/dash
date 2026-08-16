#include "drivers/pod.hpp"
#include "bsp/pins.hpp"
#include "bsp/buses.hpp"

#include "driver/i2c.h"

namespace drivers {
namespace {

constexpr i2c_port_t kPort = I2C_NUM_0;
constexpr uint8_t kIodirA = 0x00, kGppuA = 0x0C, kGpioA = 0x12, kIpolA = 0x02;

// Short, on purpose. A pod that has fallen off its connector must not stall
// the 100 Hz poll loop; it must fail fast and be counted.
constexpr int kTimeoutMs = 10;

hal::Status from_esp(int err) {
    switch (err) {
        case ESP_OK: return hal::Status::Ok;
        case ESP_ERR_TIMEOUT: return hal::Status::Timeout;
        case ESP_FAIL: return hal::Status::NotPresent;   // NACK: nobody home
        case ESP_ERR_INVALID_STATE: return hal::Status::Busy;
        default: return hal::Status::IoError;
    }
}

}  // namespace

hal::Status ButtonPod::write_reg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return from_esp(i2c_master_write_to_device(kPort, bsp::kPodAddr, buf,
                                               sizeof buf,
                                               pdMS_TO_TICKS(kTimeoutMs)));
}

hal::Status ButtonPod::read_reg(uint8_t reg, uint8_t& value) {
    return from_esp(i2c_master_write_read_device(kPort, bsp::kPodAddr, &reg, 1,
                                                 &value, 1,
                                                 pdMS_TO_TICKS(kTimeoutMs)));
}

hal::Status ButtonPod::init() {
    // The bus itself belongs to bsp (100 kHz, pull-ups, pins). This driver
    // only ever talks to a device on it.
    installed_ = true;
    return refresh_config();
}

hal::Status ButtonPod::refresh_config() {
    // Five buttons on GPA0..GPA4, wired to ground, with pull-ups. IPOL
    // inverts them in the chip, so a pressed button reads as a 1 here and
    // the polarity question is answered once, in hardware config, rather
    // than in every caller.
    hal::Status s = write_reg(kIodirA, 0xFF);
    if (!hal::ok(s)) return s;
    s = write_reg(kGppuA, 0xFF);
    if (!hal::ok(s)) return s;
    return write_reg(kIpolA, 0xFF);
}

hal::Status ButtonPod::read_buttons(uint8_t& pressed_mask) {
    uint8_t v = 0;
    const hal::Status s = read_reg(kGpioA, v);
    if (!hal::ok(s)) return s;
    pressed_mask = static_cast<uint8_t>(v & 0x1F);   // five buttons
    return hal::Status::Ok;
}

hal::Status ButtonPod::recover_bus() {
    // BRINGUP: if a slave is holding SDA low, tearing the driver down and
    // back up is not always enough — the usual fix is to bit-bang nine clock
    // pulses on SCL first to walk the slave out of its transfer. Add that
    // here once there is a pod to reproduce it on; doing it blind risks
    // driving a line the board does not expect.
    i2c_driver_delete(kPort);
    installed_ = false;
    if (!bsp::init_buses().i2c0) return hal::Status::IoError;
    return init();
}

}  // namespace drivers
