#include "core/sweep/sweep.hpp"

namespace core::sweep {
namespace {

float smoothstep(float x) { return x * x * (3.0f - 2.0f * x); }

}  // namespace

float level(uint32_t elapsed_ms, uint32_t duration_ms) {
    if (duration_ms == 0 || elapsed_ms >= duration_ms) return -1.0f;
    const float t = static_cast<float>(elapsed_ms) / static_cast<float>(duration_ms);
    return t < 0.5f ? smoothstep(t * 2.0f) : smoothstep((1.0f - t) * 2.0f);
}

bool apply(state::Frame& f, uint32_t elapsed_ms, const Config& c) {
    if (!c.enabled) return false;
    if (f.motion.best_mph > kAbortMph) return false;
    const float x = level(elapsed_ms, c.duration_ms);
    if (x < 0.0f) return false;

    const float mph = x * kPeakMph;
    // Every speed, not just the big readout: the screens decide whether a
    // speed is worth showing from the wheel and GPS figures, and at key-on
    // none of them has a reading yet.
    f.motion.best_mph = f.motion.gps_mph = mph;
    f.motion.front_mph = f.motion.rear_mph = mph;

    f.engine.rpm = x * c.peak_rpm;
    f.engine.neutral = false;
    const uint8_t gears = c.gears > 0 ? c.gears : 1;
    f.engine.gear = static_cast<uint8_t>(1 + static_cast<int>(x * (gears - 1) + 0.5f));
    return true;
}

float lamp_rpm(uint32_t elapsed_ms, float road_mph, const Config& c) {
    if (!c.enabled || road_mph > kAbortMph) return -1.0f;
    const float x = level(elapsed_ms, c.duration_ms);
    return x < 0.0f ? -1.0f : x * c.peak_rpm;
}

}  // namespace core::sweep
