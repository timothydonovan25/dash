// esp_timer behind hal::IClock.
#pragma once
#include "hal/i_clock.hpp"

namespace drivers {

class EspClock final : public hal::IClock {
public:
    uint32_t now_ms() const override;
    uint64_t now_us() const override;
};

}  // namespace drivers
