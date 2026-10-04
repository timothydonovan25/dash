// PCA9955B: sixteen constant-current LED sinks on I2C1, at 1 MHz.
//
// It drives the eight shift lamps (OUT0–7) and the panel's three backlight
// strings (OUT8–10). REXT fixes the full-scale current at 30 mA on every
// output, so no register value here can push a lamp or the backlight past
// its rating; the IREF ceilings in bsp/pins.hpp say the same thing again
// for anyone who changes REXT.
//
// Two things about this part shape the driver:
//
//  - **Power-on reset leaves every output dark** (IREF 0, PWM 0), and so
//    does a brown-out. It acknowledges and looks alive afterwards. The
//    keeper therefore re-writes the configuration registers once a second
//    (`refresh_config`), the way it did for the S3 board's expander, and
//    `ctrl` re-sends the lamp duty on its own schedule.
//  - **Lamp and backlight duty live in different registers** with different
//    writers: `ctrl` owns PWM0–7, the keeper and the render task own
//    PWM8–10, and `refresh_config` touches neither. No register has two
//    writers, so nothing here needs a lock beyond the I2C driver's own.
#pragma once
#include "hal/i_shift_lamps.hpp"

namespace drivers {

class Pca9955b final : public hal::IShiftLamps {
public:
    // Full configuration, every output at zero duty.
    hal::Status init() override;
    hal::Status set_mask(uint8_t mask) override;
    hal::Status set_master(uint8_t percent) override;

    hal::Status set_backlight(uint8_t percent);

    // MODE, LEDOUT and IREF, which a brown-out resets. Not the duty
    // registers, which have their own writers.
    hal::Status refresh_config();

private:
    hal::Status write(uint8_t reg, const uint8_t* data, size_t n);

    uint8_t mask_ = 0;
    uint8_t lamp_pwm_ = 0;
};

}  // namespace drivers
