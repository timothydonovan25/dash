#include "core/fusion/speed.hpp"
#include <cmath>

namespace core::fusion {
namespace {
// Bit index of each Source, for the per-source rejection timers.
int index_of(uint8_t bit) {
    switch (bit) {
        case kSrcAccel: return 0;
        case kSrcFront: return 1;
        case kSrcRear:  return 2;
        default:        return 3;
    }
}
}  // namespace

void SpeedFilter::start(float v, float var) {
    started_ = true;
    v_ = v < 0.0f ? 0.0f : v;
    // Keep the bias learned so far across a restart: it belongs to the
    // IMU mount, not to the moment the filter lost track.
    p_[0][0] = var;
    p_[0][1] = p_[1][0] = 0.0f;
    if (p_[1][1] <= 0.0f) p_[1][1] = 0.05f * 0.05f;
}

bool SpeedFilter::correct(float z, float r_sigma, float gate_sigma) {
    const float r = r_sigma * r_sigma;
    const float s = p_[0][0] + r;
    const float y = z - v_;
    if (y * y > gate_sigma * gate_sigma * s) return false;
    const float k0 = p_[0][0] / s;
    const float k1 = p_[1][0] / s;
    v_ += k0 * y;
    b_ += k1 * y;
    // P = (I - K H) P, with H = [1 0].
    const float p00 = p_[0][0], p01 = p_[0][1];
    p_[0][0] -= k0 * p00;
    p_[0][1] -= k0 * p01;
    p_[1][0] -= k1 * p00;
    p_[1][1] -= k1 * p01;
    return true;
}

void SpeedFilter::track(uint8_t bit, bool accepted, bool offered, uint32_t now,
                        const SpeedConfig& cfg) {
    uint32_t& since = reject_since_[index_of(bit)];
    if (!offered) return;
    if (accepted) {
        since = 0;
        out_.used |= bit;
        out_.suspect &= static_cast<uint8_t>(~bit);
        return;
    }
    out_.rejected |= bit;
    if (since == 0) since = now ? now : 1;
    if (now - since >= cfg.suspect_ms) out_.suspect |= bit;
}

SpeedOutput SpeedFilter::step(const SpeedInput& in, const SpeedConfig& cfg) {
    const uint32_t now = in.t_ms;
    out_.used = 0;
    out_.rejected = 0;

    // The first measurement anyone offers starts the filter. GPS is the
    // only source that cannot be lying about a spinning wheel, so it wins
    // a tie; then the front wheel, which does not spin under power.
    if (!started_ || now - last_t_ > cfg.max_dt_ms) {
        const bool any = (in.gps_ok && in.gps_new) || in.front_ok || in.rear_ok;
        if (!any) {
            started_ = false;
            last_t_ = now;
            out_.valid = false;
            return out_;
        }
        const float v0 = (in.gps_ok && in.gps_new) ? in.gps_mph
                       : in.front_ok ? in.front_mph : in.rear_mph;
        start(v0, 4.0f);
        last_t_ = now;
        last_fix_t_ = now;
    }

    // --- predict --------------------------------------------------------
    const float dt = static_cast<float>(now - last_t_) / 1000.0f;
    last_t_ = now;
    if (dt > 0.0f) {
        const float a = in.accel_ok ? in.accel_g : b_;   // no IMU: no change
        v_ += (a - b_) * kMphPerSecPerG * dt;
        // F = [[1, -kG dt], [0, 1]]
        const float f = -kMphPerSecPerG * dt;
        const float p00 = p_[0][0], p01 = p_[0][1], p10 = p_[1][0], p11 = p_[1][1];
        p_[0][0] = p00 + f * (p10 + p01) + f * f * p11 + cfg.q_speed * dt;
        p_[0][1] = p01 + f * p11;
        p_[1][0] = p10 + f * p11;
        p_[1][1] = p11 + cfg.q_bias * dt;
        if (!in.accel_ok) p_[0][0] += 25.0f * dt;        // blind: much less sure
        if (in.accel_ok) out_.used |= kSrcAccel;
    }

    // --- correct --------------------------------------------------------
    // GPS first: it is the referee for the wheels.
    if (in.gps_ok && in.gps_new) {
        const float h = in.hdop < 1.0f ? 1.0f : in.hdop;
        const bool ok = correct(in.gps_mph, cfg.r_gps_mph * h, cfg.gate_sigma);
        track(kSrcGps, ok, true, now, cfg);
        if (ok) {
            ref_mph_ = in.gps_mph;
            ref_t_ = now;
            have_ref_ = true;
        }
    }
    const bool refereed = have_ref_ && now - ref_t_ <= cfg.referee_ms;
    const auto plausible = [&](float w) {
        if (!refereed) return true;
        const float allow = ref_mph_ * 0.25f > cfg.wheel_gps_mph ? ref_mph_ * 0.25f
                                                                  : cfg.wheel_gps_mph;
        return std::fabs(w - ref_mph_) <= allow;
    };
    bool use_front = in.front_ok && plausible(in.front_mph);
    bool use_rear = in.rear_ok && plausible(in.rear_mph);
    if (use_front && use_rear && std::fabs(in.front_mph - in.rear_mph) > cfg.wheel_split_mph) {
        // Nearer the referee if there is one, else nearer the filter.
        const float ref = refereed ? ref_mph_ : v_;
        if (std::fabs(in.front_mph - ref) <= std::fabs(in.rear_mph - ref)) use_rear = false;
        else use_front = false;
    }
    if (in.front_ok) {
        const bool ok = use_front && correct(in.front_mph, cfg.r_wheel_mph, cfg.gate_sigma);
        track(kSrcFront, ok, true, now, cfg);
    }
    if (in.rear_ok) {
        const bool ok = use_rear && correct(in.rear_mph, cfg.r_wheel_mph, cfg.gate_sigma);
        track(kSrcRear, ok, true, now, cfg);
    }
    if (out_.used & (kSrcGps | kSrcFront | kSrcRear)) last_fix_t_ = now;

    // Everything offered was rejected for a while: the filter is the one
    // that is wrong. Restart on GPS if it is there, else the front wheel.
    const uint8_t offered = static_cast<uint8_t>((in.gps_ok ? kSrcGps : 0) |
                                                 (in.front_ok ? kSrcFront : 0) |
                                                 (in.rear_ok ? kSrcRear : 0));
    if (offered && (out_.suspect & offered) == offered) {
        const float v0 = in.gps_ok ? in.gps_mph : in.front_ok ? in.front_mph : in.rear_mph;
        start(v0, 4.0f);
        for (uint32_t& s : reject_since_) s = 0;
        out_.suspect = 0;
        last_fix_t_ = now;
        ++out_.resets;
    }

    if (v_ < 0.0f) v_ = 0.0f;              // a CRF250F does not reverse
    out_.mph = v_;
    out_.sigma_mph = std::sqrt(p_[0][0] > 0.0f ? p_[0][0] : 0.0f);
    out_.bias_g = b_;
    out_.valid = now - last_fix_t_ <= cfg.coast_ms;
    return out_;
}

}  // namespace core::fusion
