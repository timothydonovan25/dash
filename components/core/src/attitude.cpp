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

// tan and 1/cos in the Euler rates blow up at 90° of pitch, which a looped-
// out wheelie can reach. Past this the transform is held at its edge.
constexpr float kMaxEulerPitchDeg = 80.0f;

inline float deadband(float v, float band) {
    return (v > -band && v < band) ? 0.0f : v;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float wrap180(float deg) {
    while (deg >= 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

// The heading history's spacing, and how far the unwrapped heading may run
// before it is brought back by whole turns (a float keeps 0.001° there).
constexpr uint32_t kPastStepMs = 20;
constexpr float    kHeadingRebaseDeg = 7200.0f;
// Two fix intervals at 10 Hz is 200 ms; a window much longer than that is a
// fix missed, and the comparison would be too slow to catch a slide.
constexpr uint32_t kMaxSlideWindowMs = 450;

struct Vec3 { float x, y, z; };

inline Vec3 mul(const float m[3][3], Vec3 v) {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

// The bars' axes to the frame's: a turn of `steer_deg` (> 0 right) about
// the head tube, which leans back from vertical by `rake_deg`. Rodrigues'
// formula about the axis (-sin rake, 0, cos rake); a right steer is a
// negative turn about it, since z is up.
void steer_rotation(float steer_deg, float rake_deg, float r[3][3]) {
    const float ux = -std::sin(rake_deg * kDeg2Rad), uz = std::cos(rake_deg * kDeg2Rad);
    const float a = -steer_deg * kDeg2Rad;
    const float s = std::sin(a), k = 1.0f - std::cos(a);
    r[0][0] = 1.0f - k * uz * uz;  r[0][1] = -s * uz;  r[0][2] = k * ux * uz;
    r[1][0] = s * uz;              r[1][1] = 1.0f - k; r[1][2] = -s * ux;
    r[2][0] = k * ux * uz;         r[2][1] = s * ux;   r[2][2] = 1.0f - k * ux * ux;
}

}  // namespace

void Filter::set_mount_offset(float roll_deg, float pitch_deg) {
    // A board rolled by φ and pitched by θ reads the bars' vector v as
    // Rx(φ)·Ry(θ)·v, with Rx/Ry the axis turns below. Its transpose takes a
    // reading back.
    const float cr = std::cos(roll_deg * kDeg2Rad), sr = std::sin(roll_deg * kDeg2Rad);
    const float cp = std::cos(pitch_deg * kDeg2Rad), sp = std::sin(pitch_deg * kDeg2Rad);
    // (Rx·Ry)^T = Ry^T·Rx^T, with Rx = [1 0 0; 0 c s; 0 -s c] and
    // Ry = [c 0 s; 0 1 0; -s 0 c] in the x-forward, y-left, z-up frame.
    mount_[0][0] = cp;  mount_[0][1] = -sp * sr;  mount_[0][2] = -sp * cr;
    mount_[1][0] = 0;   mount_[1][1] = cr;        mount_[1][2] = -sr;
    mount_[2][0] = sp;  mount_[2][1] = cp * sr;   mount_[2][2] = cp * cr;
}

void Filter::calibrate_level(const Sample& s, float& roll_deg, float& pitch_deg) {
    // Whatever the board reads right now is defined as level. Exact for any
    // size of offset: a board at (φ, θ) reads (sin θ, cos θ sin φ, cos θ cos φ).
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
    steer_deg_ = 0.0f;
    yaw_rate_dps_ = 0.0f;
    heading_deg_ = 0.0f;
    past_n_ = past_head_ = 0;
    fixes_n_ = 0;
    slide_until_ms_ = 0;
    slide_rate_dps_ = 0.0f;
    started_ = false;
}

bool Filter::past_at(uint32_t t_ms, Past& out) const {
    // Oldest to newest, then where the filter is now.
    Past prev{};
    bool have_prev = false;
    for (size_t i = 0; i <= past_n_; ++i) {
        const Past e = i < past_n_
            ? past_[(past_head_ + kPast - past_n_ + i) % kPast]
            : Past{last_ms_, heading_deg_, steer_deg_};
        const int32_t ahead = static_cast<int32_t>(e.t_ms - t_ms);
        if (ahead >= 0) {
            if (!have_prev) {
                if (ahead != 0) return false;     // older than the history
                out = e;
                return true;
            }
            const int32_t span = static_cast<int32_t>(e.t_ms - prev.t_ms);
            const float k = span > 0 ? static_cast<float>(t_ms - prev.t_ms) / span : 1.0f;
            out.t_ms = t_ms;
            out.heading_deg = prev.heading_deg + (e.heading_deg - prev.heading_deg) * k;
            out.steer_deg = prev.steer_deg + (e.steer_deg - prev.steer_deg) * k;
            return true;
        }
        prev = e;
        have_prev = true;
    }
    return false;                                  // newer than the filter
}

void Filter::on_fix(float course_deg, float speed_mph, uint32_t t_ms, bool ok,
                    const Config& c) {
    if (!ok || !started_) { fixes_n_ = 0; return; }
    if (fixes_n_ > 0 &&
        static_cast<int32_t>(t_ms - fixes_[fixes_n_ - 1].t_ms) <= 0) return;
    if (fixes_n_ == 3) {
        fixes_[0] = fixes_[1];
        fixes_[1] = fixes_[2];
        fixes_n_ = 2;
    }
    fixes_[fixes_n_++] = Fix{t_ms, course_deg, ok};
    if (fixes_n_ < 3 || speed_mph < c.slide_min_mph) return;

    const Fix& a = fixes_[0];
    const Fix& b = fixes_[2];
    const uint32_t span_ms = b.t_ms - a.t_ms;
    if (span_ms == 0 || span_ms > kMaxSlideWindowMs) return;
    Past pa{}, pb{};
    if (!past_at(a.t_ms, pa) || !past_at(b.t_ms, pb)) return;

    // Both are heading changes over the same window: the gyro's is where
    // the bike pointed, the course's is where it went. The difference is
    // how much the sideslip grew.
    const float d_course = wrap180(b.course_deg - a.course_deg);
    const float d_heading = pb.heading_deg - pa.heading_deg;
    slide_rate_dps_ = (d_heading - d_course) * 1000.0f / static_cast<float>(span_ms);

    if (std::fabs(slide_rate_dps_) > c.slide_rate_dps) {
        // What the window taught the steering estimate was the slide.
        if (!sliding()) steer_deg_ = pa.steer_deg;
        slide_until_ms_ = last_ms_ + c.slide_hold_ms;
    }
}

void Filter::update(const Sample& s, const Config& c) {
    // Into the frame's axes: the mount offset, then the steering, both
    // turns of the sensor that the readings must be turned back out of.
    // Bias and deadband are the sensor's own, so they come first.
    const Vec3 g_sensor{deadband(s.gx_dps - bias_gx_, c.gyro_deadband_dps),
                        deadband(s.gy_dps - bias_gy_, c.gyro_deadband_dps),
                        deadband(s.gz_dps - bias_gz_, c.gyro_deadband_dps)};
    Vec3 w = mul(mount_, g_sensor);
    Vec3 a = mul(mount_, Vec3{s.ax_g, s.ay_g, s.az_g});
    const bool on_bars = c.wheelbase_ft > 0.0f;
    if (on_bars && steer_deg_ != 0.0f) {
        // The bars' turning rate itself is not removed: the estimate below
        // moves too slowly for its derivative to mean anything.
        float r[3][3];
        steer_rotation(steer_deg_, c.rake_deg, r);
        w = mul(r, w);
        a = mul(r, a);
    }

    // Body rates as roll right, nose up and yaw right.
    const float p = w.x, q = -w.y, r = -w.z;

    // What the accelerometer alone thinks. Kept for the diag screen and
    // used as the lean reference at low speed.
    accel_lean_deg_ = std::atan2(a.y, a.z) * kRad2Deg;
    const float lateral = std::sqrt(a.y * a.y + a.z * a.z);
    const float accel_pitch_deg = std::atan2(a.x, lateral) * kRad2Deg;

    if (!started_) {
        // Start from the accelerometer rather than from zero, so the dash
        // does not spend a second sweeping up to the truth at every boot.
        lean_deg_ = accel_lean_deg_;
        pitch_deg_ = accel_pitch_deg;
        lx_ = a.x;
        ly_ = a.y;
        lz_ = a.z;
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
    lx_ += (a.x - lx_) * gk;
    ly_ += (a.y - ly_) * gk;
    lz_ += (a.z - lz_) * gk;

    // Body rates to Euler rates, at the attitude so far. In a steady turn
    // the pitch gyro reads Ω·sin φ and the yaw gyro Ω·cos φ; this is what
    // puts them back together as Ω and leaves pitch still.
    const float phi = lean_deg_ * kDeg2Rad;
    const float th  = clampf(pitch_deg_, -kMaxEulerPitchDeg, kMaxEulerPitchDeg) * kDeg2Rad;
    const float sphi = std::sin(phi), cphi = std::cos(phi);
    const float turn = q * sphi + r * cphi;
    const float yaw_dps   = turn / std::cos(th);
    const float lean_dps  = p + turn * std::tan(th);
    const float pitch_dps = q * cphi - r * sphi;
    yaw_rate_dps_ = yaw_dps;

    heading_deg_ += yaw_dps * dt;
    if (std::fabs(heading_deg_) > kHeadingRebaseDeg) {
        const float turns = std::trunc(heading_deg_ / 360.0f) * 360.0f;
        heading_deg_ -= turns;
        for (Past& e : past_) e.heading_deg -= turns;
    }
    const bool slide = sliding();

    // Fast term: integrate the rates.
    float lean  = lean_deg_  + lean_dps * dt;
    float pitch = pitch_deg_ + pitch_dps * dt;

    // Slow term: pull towards a reference that does not drift.
    const float alpha = c.tau_s / (c.tau_s + dt);   // weight on the gyro
    const float max_pull = c.max_correction_dps * dt;
    const float mag = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const bool  near_1g = std::fabs(mag - 1.0f) <= c.accel_gate_g;
    const bool  moving = s.speed_mph >= c.min_speed_mph;
    const float v_ftps = s.speed_mph * kFtPerSPerMph;
    const float yaw_rps = yaw_dps * kDeg2Rad;

    float lean_ref;
    bool  have_lean_ref = true;
    if (moving && slide) {
        // The heading has left the path, so Ω is not the turn the bike is
        // balancing. The gyro carries the lean until the fixes agree again.
        have_lean_ref = false;
        lean_ref = 0.0f;
    } else if (moving) {
        // atan(v·Ω / g): the coordinated-turn reference. This is the one
        // that survives a steady corner, where the accelerometer does not.
        lean_ref = std::atan2(v_ftps * yaw_rps, kGFtPerS2) * kRad2Deg;
    } else if (near_1g) {
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
    if (near_1g) {
        const float pull = clampf((1.0f - alpha) * (accel_pitch_deg - pitch),
                                  -max_pull, max_pull);
        pitch += pull;
    }

    lean_deg_  = clampf(lean,  -89.0f, 89.0f);
    pitch_deg_ = clampf(pitch, -89.0f, 89.0f);

    // The steering angle for the next sample, from the kinematics of a
    // rolling bike: the front wheel's ground steer is atan(L·Ω/v), and a
    // leaning bike gets it from less bar, tan Δ = tan δ · cos ε / cos φ.
    // Below walking pace the yaw rate says nothing about the bars, so the
    // estimate goes back to straight ahead. In a slide it assumes a rear
    // that tracks, which is the one thing a slide is not, so it holds.
    float steer_target = slide ? steer_deg_ : 0.0f;
    if (on_bars && moving && !slide) {
        const float ground = c.wheelbase_ft * yaw_rps / v_ftps;   // tan Δ
        steer_target = std::atan(ground * std::cos(lean_deg_ * kDeg2Rad) /
                                 std::cos(c.rake_deg * kDeg2Rad)) * kRad2Deg;
        steer_target = clampf(steer_target, -c.max_steer_deg, c.max_steer_deg);
    }
    const float sk = static_cast<float>(dt_ms) / (static_cast<float>(dt_ms) + c.steer_tau_ms);
    steer_deg_ += (steer_target - steer_deg_) * sk;

    if (past_n_ == 0 ||
        s.t_ms - past_[(past_head_ + kPast - 1) % kPast].t_ms >= kPastStepMs) {
        past_[past_head_] = Past{s.t_ms, heading_deg_, steer_deg_};
        past_head_ = (past_head_ + 1) % kPast;
        if (past_n_ < kPast) ++past_n_;
    }
}

LevelCal::Result LevelCal::feed(const Sample& raw) {
    if (!active_) return Result::Idle;
    const float mag = std::sqrt(raw.ax_g * raw.ax_g + raw.ay_g * raw.ay_g +
                                raw.az_g * raw.az_g);
    if (std::fabs(mag - 1.0f) > kStillG || std::fabs(raw.gx_dps) > kStillDps ||
        std::fabs(raw.gy_dps) > kStillDps || std::fabs(raw.gz_dps) > kStillDps) {
        active_ = false;
        return Result::Moved;
    }
    sum_.ax += raw.ax_g;  sum_.ay += raw.ay_g;  sum_.az += raw.az_g;
    sum_.gx += raw.gx_dps; sum_.gy += raw.gy_dps; sum_.gz += raw.gz_dps;
    if (++n_ < kSamples) return Result::Pending;
    const double k = 1.0 / static_cast<double>(n_);
    mean_ = Sample{};
    mean_.ax_g = static_cast<float>(sum_.ax * k);
    mean_.ay_g = static_cast<float>(sum_.ay * k);
    mean_.az_g = static_cast<float>(sum_.az * k);
    mean_.gx_dps = static_cast<float>(sum_.gx * k);
    mean_.gy_dps = static_cast<float>(sum_.gy * k);
    mean_.gz_dps = static_cast<float>(sum_.gz * k);
    mean_.t_ms = raw.t_ms;
    active_ = false;
    return Result::Done;
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
