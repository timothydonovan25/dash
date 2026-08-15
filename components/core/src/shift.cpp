#include "core/shift/shift.hpp"

namespace core::shift {

uint16_t threshold(const Config& c, int lamp) {
    if (lamp <= 0) return c.base_rpm;
    if (lamp >= kLampCount - 1) return static_cast<uint16_t>(c.base_rpm + c.span_rpm);
    // Even spacing across the span, integer maths, no rounding drift.
    const uint32_t step = static_cast<uint32_t>(c.span_rpm) * lamp / (kLampCount - 1);
    return static_cast<uint16_t>(c.base_rpm + step);
}

uint8_t pattern(uint16_t rpm, const Config& c, uint8_t prev) {
    uint8_t mask = 0;
    for (int i = 0; i < kLampCount; ++i) {
        const uint16_t on_at = threshold(c, i);
        const bool was_on = (prev >> i) & 1u;
        // A lamp that is already on has a lower bar to stay on.
        const uint16_t bar = was_on
            ? (on_at > c.hysteresis_rpm ? static_cast<uint16_t>(on_at - c.hysteresis_rpm) : 0)
            : on_at;
        if (rpm >= bar) mask |= static_cast<uint8_t>(1u << i);
    }
    return mask;
}

bool over_limit(uint16_t rpm, const Config& c) { return rpm >= c.flash_rpm; }

}  // namespace core::shift
