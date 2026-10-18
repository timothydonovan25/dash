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

uint16_t lamp_rpm(float rpm, float rpm_per_s, const Config& c) {
    if (rpm < 0.0f) rpm = 0.0f;
    float lead = 0.0f;
    const float climb = rpm_per_s - static_cast<float>(c.lead_floor_rpm_s);
    if (c.lead_ms > 0 && rpm > 0.0f && climb > 0.0f) {
        lead = climb * static_cast<float>(c.lead_ms) * 1e-3f;
        if (lead > c.lead_cap_rpm) lead = c.lead_cap_rpm;
    }
    const float r = rpm + lead;
    return r >= 65535.0f ? 65535 : static_cast<uint16_t>(r);
}

float PeakHold::update(float rpm, uint32_t t_ms, uint32_t hold_ms, float gap_rpm) {
    if (hold_ms == 0) {
        reset();
        return 0.0f;
    }
    if (rpm >= peak_) {
        // Climbing, or level: this is the peak now.
        peak_ = rpm;
        at_ms_ = t_ms;
        released_ = false;
    } else if (released_ || t_ms - at_ms_ > hold_ms) {
        // Let go: follow it down until it climbs again.
        released_ = true;
        peak_ = rpm;
    }
    return !released_ && peak_ - rpm >= gap_rpm ? peak_ : 0.0f;
}

void PeakHold::reset() {
    peak_ = 0.0f;
    at_ms_ = 0;
    released_ = true;
}

uint8_t hold_lamp(uint16_t peak_rpm, const Config& c) {
    if (peak_rpm >= c.flash_rpm) return static_cast<uint8_t>(1u << (kLampCount - 1));
    for (int i = kLampCount - 1; i >= 0; --i) {
        if (peak_rpm >= threshold(c, i)) return static_cast<uint8_t>(1u << i);
    }
    return 0;
}

}  // namespace core::shift
