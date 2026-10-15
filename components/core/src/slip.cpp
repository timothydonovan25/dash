#include "core/slip/slip.hpp"

namespace core::slip {

namespace {
uint8_t quantised_pct(float fraction) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    const int p = static_cast<int>(fraction * 100.0f + 2.5f) / 5 * 5;
    return static_cast<uint8_t>(p > 100 ? 100 : p);
}
}  // namespace

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::None:      return "";
        case Kind::Spin:      return "SPIN";
        case Kind::LockFront: return "FRONT LOCK";
        case Kind::LockRear:  return "REAR LOCK";
    }
    return "";
}

Reading Meter::update(float front_mph, float rear_mph, float road_mph, bool road_ok,
                      uint32_t t_ms, const Config& c) {
    Reading now;
    // Without road speed, the front is the best stand-in for spin: it is
    // not driven. A lock cannot be told from a stop that way.
    const float ref = road_ok ? road_mph : front_mph;
    if (ref >= c.min_mph) {
        const float spin = (rear_mph - ref) / ref;
        const float front_lock = road_ok ? (ref - front_mph) / ref : 0.0f;
        const float rear_lock = road_ok ? (ref - rear_mph) / ref : 0.0f;
        // A lock outranks a spin: it is the one that puts the bike down.
        if (front_lock >= c.lock_start && front_lock >= rear_lock) {
            now.kind = Kind::LockFront;
            now.pct = quantised_pct(front_lock);
        } else if (rear_lock >= c.lock_start) {
            now.kind = Kind::LockRear;
            now.pct = quantised_pct(rear_lock);
        } else if (spin >= c.spin_start) {
            now.kind = Kind::Spin;
            now.pct = quantised_pct(spin);
        }
    }

    if (now.kind != Kind::None) {
        // Hold the worst of a run, so the bar shows the peak, not the tail.
        if (held_.kind != now.kind || now.pct >= held_.pct ||
            static_cast<int32_t>(t_ms - held_until_ms_) >= 0) {
            held_ = now;
        }
        held_until_ms_ = t_ms + c.hold_ms;
        return held_;
    }
    if (held_.kind != Kind::None && static_cast<int32_t>(t_ms - held_until_ms_) < 0) {
        return held_;
    }
    held_ = Reading{};
    return held_;
}

}  // namespace core::slip
