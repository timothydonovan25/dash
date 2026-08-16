#include "core/gear/gear.hpp"

namespace core::gear {
namespace {
inline float fabsf_(float v) { return v < 0.0f ? -v : v; }
}  // namespace

void Estimator::reset() {
    gear_ = kUnknown;
    ratio_ = 0.0f;
    started_ = false;
}

uint8_t Estimator::update(float rpm, float speed_mph, bool neutral,
                          uint32_t t_ms, const Config& c) {
    if (!started_) {
        last_good_ms_ = t_ms;
        started_ = true;
    }

    // Neutral is sensed, not inferred, so it wins outright. It also clears
    // the hold: coming out of neutral should not briefly show whatever gear
    // the bike was in before it stopped.
    if (neutral) {
        gear_ = kUnknown;
        ratio_ = 0.0f;
        last_good_ms_ = t_ms;
        return gear_;
    }

    const bool moving = speed_mph >= c.min_speed_mph && rpm >= c.min_rpm;
    if (moving) {
        ratio_ = rpm / speed_mph;

        uint8_t best = kUnknown;
        float best_err = 0.0f;
        const uint8_t n = c.count < kMaxGears ? c.count : kMaxGears;
        for (uint8_t i = 0; i < n; ++i) {
            const float want = c.rpm_per_mph[i];
            if (want <= 0.0f) continue;
            const float err = fabsf_(ratio_ - want) / want * 100.0f;
            if (best == kUnknown || err < best_err) {
                best_err = err;
                best = static_cast<uint8_t>(i + 1);
            }
        }

        if (best != kUnknown && best_err <= c.tolerance_pct) {
            gear_ = best;
            last_good_ms_ = t_ms;
            return gear_;
        }
    } else {
        ratio_ = 0.0f;
    }

    // Nothing matched. Hold the last gear briefly — a shift is a clutch pull
    // and two ambiguous readings, and blanking the tile through every gear
    // change is worse than being a few hundred milliseconds stale. After
    // that, admit it does not know.
    if (t_ms - last_good_ms_ > c.hold_ms) gear_ = kUnknown;
    return gear_;
}

}  // namespace core::gear
