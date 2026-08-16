#include "drivers/clock.hpp"
#include "esp_timer.h"

namespace drivers {

uint64_t EspClock::now_us() const {
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint32_t EspClock::now_ms() const {
    // Truncated to 32 bits deliberately. Everything downstream compares with
    // unsigned differences, so the 49.7-day wrap is a non-event, and a
    // 32-bit millisecond count is half the width to publish and log.
    return static_cast<uint32_t>(now_us() / 1000ull);
}

}  // namespace drivers
