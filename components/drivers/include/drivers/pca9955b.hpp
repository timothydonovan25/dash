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
//    does a brown-out. It acknowledges and looks alive afterwards. `ctrl`
//    therefore re-writes the configuration registers once a second
//    (`refresh_config`), the way the keeper did for the S3 board's
//    expander, and re-sends the lamp and backlight duty on its own schedule.
//  - **One task writes it.** Every call here is made from `ctrl`, so I2C1
//    has one user and a lamp write never waits for a core-1 task that was
//    preempted while holding the bus. The backlight level is decided by the
//    keeper and handed over through Deps::backlight_pct. `mask_` and
//    `lamp_pwm_` are that one task's state and nobody else touches them.
//
//    The one exception is `power` at the moment VIN is cut: it calls
//    `blank_lamps` and `set_backlight(0)` directly, because the hold-up is
//    too short to wait for a tick. Neither touches `mask_` or `lamp_pwm_`,
//    and the i2c_master driver's bus lock keeps the two transfers apart.
#pragma once
#include "hal/i_shift_lamps.hpp"

namespace drivers {

class Pca9955b final : public hal::IShiftLamps {
public:
    // The I2C1 bus from bsp (BusStatus::i2c1, an i2c_master_bus_handle_t).
    // Call before init(); without one every call says NotPresent.
    void attach(void* bus) { bus_ = bus; }

    // Full configuration, every output at zero duty. Adds the device to the
    // bus the first time.
    hal::Status init() override;
    hal::Status set_mask(uint8_t mask) override;
    hal::Status set_master(uint8_t percent) override;

    hal::Status set_backlight(uint8_t percent);

    // OUT0..7 to zero duty, leaving `mask_` and `lamp_pwm_` alone: the
    // power-fail path, from another task (see above).
    hal::Status blank_lamps();

    // MODE, LEDOUT and IREF, which a brown-out resets. Not the duty
    // registers, which have their own writers. The MODE2 write also clears
    // the error flags (CLRERR), so each read_faults() sees only what the
    // chip found since the last refresh.
    hal::Status refresh_config();

    // MODE2 (OVERTEMP, ERROR) and EFLAG0..3, raw. core::shift::
    // decode_led_faults() says what they mean; this only fetches them.
    hal::Status read_faults(uint8_t& mode2, uint8_t eflag[4]);

private:
    hal::Status write(uint8_t reg, const uint8_t* data, size_t n);
    hal::Status read(uint8_t reg, uint8_t* data, size_t n);

    void*   bus_ = nullptr;   // i2c_master_bus_handle_t
    void*   dev_ = nullptr;   // i2c_master_dev_handle_t
    uint8_t mask_ = 0;
    uint8_t lamp_pwm_ = 0;
};

}  // namespace drivers
