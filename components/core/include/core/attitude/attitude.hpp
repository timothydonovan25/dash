// Lean and pitch from a 6-axis IMU (§8). Pure maths on a sample stream.
//
// The thing to understand before touching this file: **an accelerometer
// cannot measure lean on a motorcycle.** In a steady turn the bike leans
// into the resultant of gravity and centripetal force, so the accelerometer
// still reads "down" through the bike and shows a few degrees at 40° of
// real lean. Every naive lean gauge gets this wrong.
//
// What works is integrating the roll-rate gyro for the fast term and
// correcting the drift with a reference that does not care about the
// resultant:
//
//     lean ≈ atan( v · ω_yaw / g )
//
// A bike in a steady turn is, to a very good approximation, in coordinated
// balance — that is the same physics that makes it feel level to the rider.
// Above about 5 mph this is good to a few degrees. Below that the term
// collapses (v → 0) and the accelerometer becomes the better reference,
// because a nearly stationary bike is not turning.
//
// Axis convention, which the IMU driver is responsible for satisfying:
//
//   x forward, y right, z up, all in the bike's frame after mount offset.
//   Upright and at rest:      ax = 0,      ay = 0,       az = +1 g
//   Leaning right by θ:       ay = +sin θ, az = +cos θ
//   gx_dps > 0 while lean is increasing to the right.
//   gz_dps > 0 while turning right.
//
// Check it on the bench with the DIAG IMU screen before trusting a number
// off this filter. A sign error here reads as a perfectly smooth, perfectly
// wrong lean angle.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::attitude {

struct Config {
    // Complementary time constant. Larger trusts the gyro for longer, which
    // is smoother and drifts more. 1 s is a reasonable start for a part with
    // this bias stability.
    float tau_s = 1.0f;

    // Below this the v·ω reference is meaningless and the accelerometer is
    // used instead (§8).
    float min_speed_mph = 5.0f;

    // Trust the accelerometer for pitch only when the total specific force
    // is near 1 g. Hard acceleration and braking tilt the vector and would
    // otherwise be read as the bike pitching.
    float accel_gate_g = 0.15f;

    // Ceiling on how fast the correction may pull the estimate, in degrees
    // per second. Without it a one-sample accelerometer spike from a rock
    // strike snaps the lean gauge across the screen.
    float max_correction_dps = 45.0f;

    // Gyro readings below this are treated as zero, so a bike on the stand
    // does not slowly roll over. Set from the measured noise floor.
    float gyro_deadband_dps = 0.15f;
};

struct Sample {
    float    gx_dps = 0.0f;    // roll rate
    float    gy_dps = 0.0f;    // pitch rate
    float    gz_dps = 0.0f;    // yaw rate
    float    ax_g   = 0.0f;
    float    ay_g   = 0.0f;
    float    az_g   = 1.0f;
    float    speed_mph = 0.0f; // GPS speed; the correction term needs it
    uint32_t t_ms   = 0;
};

class Filter {
public:
    // The board does not sit level in the bike. Without this every lean
    // reading carries a fixed bias. Set from NVS at boot.
    void set_mount_offset(float roll_deg, float pitch_deg);

    // The ImuCalibrate command: park upright, hold E. Takes the current
    // gravity vector as the new zero. Returns the offsets it stored so the
    // caller can write them to NVS.
    void calibrate_level(const Sample& s, float& roll_deg, float& pitch_deg);

    // Zero the gyro bias from a stationary average. Called during boot while
    // the bike is still, and after any calibrate.
    void set_gyro_bias(float gx_dps, float gy_dps, float gz_dps);

    void update(const Sample& s, const Config& c);
    void reset();

    float lean_deg() const  { return lean_deg_; }
    float pitch_deg() const { return pitch_deg_; }

    // What the accelerometer alone would say. Not used for lean — it is on
    // the DIAG IMU screen precisely so the difference is visible, which is
    // the quickest way to prove the filter is doing something.
    float accel_lean_deg() const { return accel_lean_deg_; }

    // Fills the lean, pitch, yaw-rate and g fields. The jump fields belong
    // to core/jump and are left alone.
    void publish_into(state::Attitude& a) const;

private:
    float lean_deg_        = 0.0f;
    float pitch_deg_       = 0.0f;
    float accel_lean_deg_  = 0.0f;
    float roll_offset_deg_ = 0.0f;
    float pitch_offset_deg_= 0.0f;
    float bias_gx_ = 0.0f, bias_gy_ = 0.0f, bias_gz_ = 0.0f;

    float ax_ = 0.0f, ay_ = 0.0f, az_ = 1.0f;
    float yaw_rate_dps_ = 0.0f;

    uint32_t last_ms_ = 0;
    bool     started_ = false;
};

}  // namespace core::attitude
