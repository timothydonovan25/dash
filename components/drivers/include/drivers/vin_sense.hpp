// VIN sense: one divider into ADC1, read in one-shot mode.
//
// Reports millivolts at the pin and nothing else. What VIN that means, and
// whether it is the bike or USB, is core::power's job.
#pragma once
#include "hal/i_rails.hpp"

namespace drivers {

class VinSense final : public hal::IRailSense {
public:
    hal::Status init() override;
    hal::Status read_vin_pin_mv(uint32_t& mv) override;

private:
    void* unit_ = nullptr;    // adc_oneshot_unit_handle_t
    void* cali_ = nullptr;    // adc_cali_handle_t, null if uncalibrated
    int   channel_ = -1;
};

}  // namespace drivers
