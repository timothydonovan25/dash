#include "core/attitude/attitude.hpp"
#include <cmath>

namespace core::attitude {
namespace {

constexpr float kPi      = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;
constexpr float kGFtPerS2= 32.174f;
constexpr float kFtPerSPerMph = 1.46666667f;

// A tick this long means samples were dropped. Integrating a gyro across a
// gap invents rotation that never happened, so the gap is skipped and the
// estimate simply stays where it was.
constexpr uint32_t kMaxTickMs = 200;

inline float deadband(float v, float band) {
    return (v > -band && v < band) ? 0.0f : v;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

void Filter::set_mount_offset(float roll_deg, float pitch_deg) {
    roll_offset_deg_ = roll_deg;
    pitch_offset_deg_ = pitch_deg;
}

void Filter::calibrate_level(const Sample& s, float& roll_deg, float& pitch_deg) {
    // Whatever the board reads right now is defined as level.
    roll_deg  = std::atan2(s.ay_g, s.az_g) * kRad2Deg;
    const float lateral = std::sqrt(s.ay_g * s.ay_g + s.az_g * s.az_g);
    pitch_deg = std::atan2(s.ax_g, lateral) * kRad2Deg;
    set_mount_offset(roll_deg, pitch_deg);
    reset();
}

void Filter::set_gyro_bias(float gx_dps, float gy_dps, float gz_dps) {
    bias_gx_ = gx_dps;
    bias_gy_ = gy_dps;
    bias_gz_ = gz_dps;
}

void Filter::reset() {
    lean_deg_ = 0.0f;
    pitch_deg_ = 0.0f;
    started_ = false;
}

void Filter::update(const Sample& s, const Config& c) {
    ax_ = s.ax_g;
    ay_ = s.ay_g;
    az_ = s.az_g;

    const float gx = deadband(s.gx_dps - bias_gx_, c.gyro_deadband_dps);
    const float gy = deadband(s.gy_dps - bias_gy_, c.gyro_deadband_dps);
    const float gz = deadband(s.gz_dps - bias_gz_, c.gyro_deadband_dps);
    yaw_rate_dps_ = gz;

    // What the accelerometer alone thinks, mount offset removed. Kept for
    // the diag screen and used as the lean reference at low speed.
    accel_lean_deg_ = std::atan2(ay_, az_) * kRad2Deg - roll_offset_deg_;
    const float lateral = std::sqrt(ay_ * ay_ + az_ * az_);
    const float accel_pitch_deg =
        std::atan2(ax_, lateral) * kRad2Deg - pitch_offset_deg_;

    if (!started_) {
        // Start from the accelerometer rather than from zero, so the dash
        // does not spend a second sweeping up to the truth at every boot.
        lean_deg_ = accel_lean_deg_;
        pitch_deg_ = accel_pitch_deg;
        lx_ = ax_;
        ly_ = ay_;
        lz_ = az_;
        last_ms_ = s.t_ms;
        started_ = true;
        return;
    }

    const uint32_t dt_ms = s.t_ms - last_ms_;      // unsigned: wrap-safe
    last_ms_ = s.t_ms;
    if (dt_ms == 0 || dt_ms > kMaxTickMs) return;
    const float dt = static_cast<float>(dt_ms) / 1000.0f;

    // The published g vector: dt / (dt + tau) is the first-order low-pass.
    const float gk = static_cast<float>(dt_ms) / (static_cast<float>(dt_ms) + c.g_tau_ms);
    lx_ += (ax_ - lx_) * gk;
    ly_ += (ay_ - ly_) * gk;
    lz_ += (az_ - lz_) * gk;

    // Fast term: integrate the rate gyros.
    float lean  = lean_deg_  + gx * dt;
    float pitch = pitch_deg_ + gy * dt;

    // Slow term: pull towards a reference that does not drift.
    const float alpha = c.tau_s / (c.tau_s + dt);   // weight on the gyro
    const float max_pull = c.max_correction_dps * dt;

    float lean_ref;
    bool  have_lean_ref = true;
    if (s.speed_mph >= c.min_speed_mph) {
        // atan(v·ω / g): the coordinated-turn reference. This is the one
        // that survives a steady corner, where the accelerometer does not.
        const float v_ftps = s.speed_mph * kFtPerSPerMph;
        const float yaw_rps = gz * kDeg2Rad;
        lean_ref = std::atan2(v_ftps * yaw_rps, kGFtPerS2) * kRad2Deg;
    } else if (std::fabs(std::sqrt(ax_ * ax_ + ay_ * ay_ + az_ * az_) - 1.0f)
               <= c.accel_gate_g) {
        // Slow or stopped: the bike is not generating lateral force, so
        // "down" really is down.
        lean_ref = accel_lean_deg_;
    } else {
        have_lean_ref = false;
        lean_ref = 0.0f;
    }

    if (have_lean_ref) {
        const float pull = clampf((1.0f - alpha) * (lean_ref - lean),
                                  -max_pull, max_pull);
        lean += pull;
    }

    // Pitch corrects against gravity, but only when the total force is near
    // 1 g. Under hard acceleration the vector tips backwards and would be
    // read as the front wheel lifting.
    const float mag = std::sqrt(ax_ * ax_ + ay_ * ay_ + az_ * az_);
    if (std::fabs(mag - 1.0f) <= c.accel_gate_g) {
        const float pull = clampf((1.0f - alpha) * (accel_pitch_deg - pitch),
                                  -max_pull, max_pull);
        pitch += pull;
    }

    lean_deg_  = clampf(lean,  -89.0f, 89.0f);
    pitch_deg_ = clampf(pitch, -89.0f, 89.0f);
}

void Filter::publish_into(state::Attitude& a) const {
    a.lean_deg     = lean_deg_;
    a.pitch_deg    = pitch_deg_;
    a.yaw_rate_dps = yaw_rate_dps_;
    a.g_lat        = ly_;
    a.g_lon        = lx_;
    a.g_vert       = lz_;
}

}  // namespace core::attitude
