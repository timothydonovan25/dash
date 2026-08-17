#include "drivers/shift_lamps.hpp"
#include "drivers/mcp23s17.hpp"
#include "bsp/pins.hpp"
#include "core/shift/shift.hpp"

#include "driver/ledc.h"

namespace drivers {

// The mask that core/shift produces has to fit the port the lamps are on.
// Both numbers already existed and neither referenced the other, so nothing
// would have noticed them drifting apart.
static_assert(core::shift::kLampCount == bsp::mcp::kShiftCount,
              "core/shift and the MCP23S17 port disagree about lamp count");
static_assert(bsp::mcp::kShift0 == 0,
              "the lamp mask goes straight to the port; lamp 1 must be bit 0");

namespace {
// Channel 1: the backlight already has channel 0 on the same timer.
constexpr ledc_timer_t   kTimer   = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
constexpr ledc_mode_t    kMode    = LEDC_LOW_SPEED_MODE;
}  // namespace

hal::Status ShiftLamps::init() {
    if (!ledc_ready_) {
        // The timer is shared with the backlight and configured there; this
        // only claims a channel on it.
        ledc_channel_config_t c = {};
        c.speed_mode = kMode;
        c.channel = kChannel;
        c.timer_sel = kTimer;
        c.gpio_num = bsp::kShiftMasterPwm;
        c.duty = 0;
        if (ledc_channel_config(&c) != ESP_OK) return hal::Status::IoError;
        ledc_ready_ = true;
    }
    // Lamps off, gate closed. `ctrl` opens it once it knows whether the
    // engine is running, and therefore how much current is available.
    set_mask(0);
    return set_master(0);
}

hal::Status ShiftLamps::set_mask(uint8_t mask) {
    if (!mcp_) return hal::Status::NotPresent;
    // Straight through: core::shift::pattern() returns bit 0 = lamp 1, and
    // the lamps are wired GPA0..GPA7 in that order. One SPI write, about
    // 20 us, and it is the last link in the 3 ms tach-to-lamp budget.
    return mcp_->write_shift_lamps(mask);
}

hal::Status ShiftLamps::set_master(uint8_t percent) {
    if (!ledc_ready_) return hal::Status::NotPresent;
    if (percent > 100) percent = 100;
    percent_ = percent;
    const uint32_t duty = (255u * percent) / 100u;
    if (ledc_set_duty(kMode, kChannel, duty) != ESP_OK) return hal::Status::IoError;
    return ledc_update_duty(kMode, kChannel) == ESP_OK ? hal::Status::Ok
                                                       : hal::Status::IoError;
}

}  // namespace drivers
