#include "core/gear/gear.hpp"

namespace core::gear {
namespace {
inline float abs_(float v) { return v < 0.0f ? -v : v; }
}  // namespace

float ratio(float rpm, float speed_mph, const Config& c) {
    if (speed_mph < c.min_speed_mph || rpm < c.min_rpm) return 0.0f;
    return rpm / speed_mph;
}

uint8_t estimate(float rpm, float speed_mph, bool neutral, const Config& c) {
    // Neutral is sensed rather than inferred, so it is not this function's
    // business — the caller shows N and never asks.
    if (neutral) return kUnknown;

    const float r = ratio(rpm, speed_mph, c);
    if (r <= 0.0f) return kUnknown;

    // Whichever gear's ratio is nearest, in proportion rather than in
    // absolute rpm-per-mph: the gears are far apart at the bottom of the box
    // and close together at the top, so a fixed rpm-per-mph margin would
    // favour first and starve fifth.
    uint8_t best = kUnknown;
    float best_err = 0.0f;
    const uint8_t n = c.count < kMaxGears ? c.count : kMaxGears;
    for (uint8_t i = 0; i < n; ++i) {
        const float want = c.rpm_per_mph[i];
        if (want <= 0.0f) continue;          // an unconfigured slot
        const float err = abs_(r - want) / want;
        if (best == kUnknown || err < best_err) {
            best_err = err;
            best = static_cast<uint8_t>(i + 1);
        }
    }
    return best;
}

}  // namespace core::gear
