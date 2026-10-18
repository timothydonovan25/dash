#include "core/session/session.hpp"

namespace core::session {
namespace {
// The trip computer's rule: a tick this long means `logic` was starved, and
// charging it with a whole second of riding would invent some.
constexpr uint32_t kMaxTickMs = 1000;
}  // namespace

void Tracker::update(const Input& in) {
    // A landing is counted once, whenever it is seen: `land_ms` holds until
    // the next one. The first pass takes whatever is already there as seen,
    // so a jump from before this tracker started is not this session's.
    if (!started_) {
        started_ = true;
        last_ms_ = in.t_ms;
        seen_land_ms_ = in.land_ms;
        return;
    }
    if (in.imu_ok && in.land_ms != seen_land_ms_) {
        seen_land_ms_ = in.land_ms;
        if (in.land_ms != 0) {
            if (jumps_ != UINT16_MAX) ++jumps_;
            if (in.airtime_ms > best_air_ms_) best_air_ms_ = in.airtime_ms;
            if (in.land_g > hardest_land_g_) hardest_land_g_ = in.land_g;
        }
    }

    const uint32_t dt_ms = in.t_ms - last_ms_;
    last_ms_ = in.t_ms;
    if (dt_ms == 0 || dt_ms > kMaxTickMs) return;
    const double dt_s = static_cast<double>(dt_ms) / 1000.0;

    if (in.speed_mph > 0.0f) {
        dist_mi_ += static_cast<double>(in.speed_mph) * dt_s / 3600.0;
        if (in.speed_mph > top_mph_) top_mph_ = in.speed_mph;
    }
    if (in.speed_mph >= kMovingMph) moving_s_ += dt_s;
    if (in.engine_on) engine_s_ += dt_s;
}

state::Session Tracker::publish() const {
    state::Session s;
    s.dist_mi = static_cast<float>(dist_mi_);
    s.moving_s = static_cast<uint32_t>(moving_s_);
    s.engine_s = static_cast<uint32_t>(engine_s_);
    s.top_mph = top_mph_;
    s.avg_mph = moving_s_ > 0.0 ? static_cast<float>(dist_mi_ * 3600.0 / moving_s_) : 0.0f;
    s.best_air_ms = best_air_ms_;
    s.hardest_land_g = hardest_land_g_;
    s.jumps = jumps_;
    return s;
}

}  // namespace core::session
