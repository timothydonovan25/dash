#include "drivers/pca9955b.hpp"
#include "bsp/pins.hpp"
#include "core/shift/shift.hpp"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"

namespace drivers {

// The mask from core/shift goes straight onto OUT0..OUT7.
static_assert(core::shift::kLampCount == bsp::kLedLampCount,
              "core/shift and the LED driver disagree about lamp count");
static_assert(bsp::kLedLamp0 == 0,
              "the lamp mask maps bit n to OUTn; lamp 1 must be OUT0");
static_assert(bsp::kLedBacklight0 + bsp::kLedBacklightCount <= 16 &&
              bsp::kLedBacklight0 >= bsp::kLedLamp0 + bsp::kLedLampCount,
              "backlight channels must sit above the lamps, inside OUT0..15");

namespace {

constexpr i2c_port_t kPort = I2C_NUM_1;
constexpr TickType_t kTimeout = pdMS_TO_TICKS(10);

// Register map, datasheet Rev. 2.3 Table 7.
constexpr uint8_t kMode1   = 0x00;
constexpr uint8_t kPwm0    = 0x08;
constexpr uint8_t kIref0   = 0x18;
// Bit 7 of the register byte sets the auto-increment flag. With MODE1's
// AI1:AI0 left at 00 the pointer runs 00h..43h, so one transfer covers any
// contiguous run of registers.
constexpr uint8_t kAutoInc = 0x80;

// LEDOUTn: two bits per output. 10b is "individual PWM", which is the only
// mode used; 00b is off for the outputs nothing is wired to.
constexpr uint8_t kLdrPwm  = 0b10;

uint8_t duty_of(uint8_t percent) {
    if (percent > 100) percent = 100;
    return static_cast<uint8_t>((255u * percent) / 100u);
}

}  // namespace

hal::Status Pca9955b::write(uint8_t reg, const uint8_t* data, size_t n) {
    // The largest run written is sixteen registers plus the address byte.
    uint8_t buf[1 + 16];
    if (n > 16) return hal::Status::Unsupported;
    buf[0] = static_cast<uint8_t>(kAutoInc | reg);
    for (size_t i = 0; i < n; ++i) buf[1 + i] = data[i];
    const esp_err_t err =
        i2c_master_write_to_device(kPort, bsp::kLedAddr, buf, n + 1, kTimeout);
    if (err == ESP_OK) return hal::Status::Ok;
    return err == ESP_ERR_TIMEOUT ? hal::Status::Timeout : hal::Status::IoError;
}

hal::Status Pca9955b::refresh_config() {
    // MODE1 = 0: normal mode, auto-increment over the whole map, and no
    // answer to the sub-call or all-call addresses — this is the only
    // device on its bus, and a group address nobody uses is just another
    // way to write it by accident. MODE2 = 0: outputs change on STOP, so the
    // eight lamp duties in one transfer land together.
    uint8_t head[6] = {0x00, 0x00, 0, 0, 0, 0};
    for (int ch = 0; ch < 16; ++ch) {
        const bool lamp = ch >= bsp::kLedLamp0 &&
                          ch < bsp::kLedLamp0 + bsp::kLedLampCount;
        const bool back = ch >= bsp::kLedBacklight0 &&
                          ch < bsp::kLedBacklight0 + bsp::kLedBacklightCount;
        if (lamp || back) head[2 + ch / 4] |= static_cast<uint8_t>(kLdrPwm << (2 * (ch % 4)));
    }
    hal::Status s = write(kMode1, head, sizeof head);
    if (!hal::ok(s)) return s;

    uint8_t iref[16] = {};
    for (int i = 0; i < bsp::kLedLampCount; ++i) {
        iref[bsp::kLedLamp0 + i] = bsp::kLedLampIrefMax;
    }
    for (int i = 0; i < bsp::kLedBacklightCount; ++i) {
        iref[bsp::kLedBacklight0 + i] = bsp::kLedBacklightIrefMax;
    }
    return write(kIref0, iref, sizeof iref);
}

hal::Status Pca9955b::init() {
    // Dark first, then configured: if the configuration write fails half
    // way, nothing is left lit at a duty nobody chose.
    const uint8_t zero[16] = {};
    hal::Status s = write(kPwm0, zero, sizeof zero);
    if (!hal::ok(s)) return s;
    mask_ = 0;
    lamp_pwm_ = 0;
    return refresh_config();
}

hal::Status Pca9955b::set_mask(uint8_t mask) {
    // One transfer, OUT0..OUT7: 10 bytes, about 0.1 ms at 1 MHz. This is the
    // last link in the 3 ms tach-to-lamp budget.
    mask_ = mask;
    uint8_t pwm[bsp::kLedLampCount];
    for (int i = 0; i < bsp::kLedLampCount; ++i) {
        pwm[i] = (mask >> i) & 1u ? lamp_pwm_ : 0;
    }
    return write(static_cast<uint8_t>(kPwm0 + bsp::kLedLamp0), pwm, sizeof pwm);
}

hal::Status Pca9955b::set_master(uint8_t percent) {
    lamp_pwm_ = duty_of(percent);
    return set_mask(mask_);
}

hal::Status Pca9955b::set_backlight(uint8_t percent) {
    const uint8_t d = duty_of(percent);
    uint8_t pwm[bsp::kLedBacklightCount];
    for (int i = 0; i < bsp::kLedBacklightCount; ++i) pwm[i] = d;
    return write(static_cast<uint8_t>(kPwm0 + bsp::kLedBacklight0), pwm, sizeof pwm);
}

}  // namespace drivers
