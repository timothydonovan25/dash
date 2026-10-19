// Lean and pitch from a 6-axis IMU (§8). Pure maths on a sample stream.
//
// The thing to understand before touching this file: **an accelerometer
// cannot measure lean on a motorcycle.** In a steady turn the bike leans
// into the resultant of gravity and centripetal force, so the accelerometer
// still reads "down" through the bike and shows a few degrees at 40° of
// real lean. Every naive lean gauge gets this wrong.
//
// What works is integrating the gyros for the fast term and correcting the
// drift with a reference that does not care about the resultant:
//
//     lean ≈ atan( v · Ω / g )
//
// where Ω is the yaw rate about the *world's* vertical. The yaw gyro is not
// that: it turns with the bike, so at lean φ it reads only Ω·cos φ and the
// rest lands on the pitch gyro, Ω·sin φ. Using the yaw gyro as Ω read 33° in
// a 40° turn, and integrating the pitch gyro as pitch rate climbed to the
// 89° clamp in any corner past 30°. Both go through the Euler-rate
// transform below instead.
//
// A bike in a steady turn is, to a very good approximation, in coordinated
// balance — that is the same physics that makes it feel level to the rider.
// Above about 5 mph this is good to a few degrees. Below that the term
// collapses (v → 0) and the accelerometer becomes the better reference,
// because a nearly stationary bike is not turning. It measures the lean of
// bike and rider together: a rider who pushes the bike down under them, and
// the tyre's width, make the bike lean further than this says.
//
// **The IMU is on the handlebar**, so it also turns with the steering, about
// a head tube raked back by `rake_deg`. A turned bar reads as lean (about
// 0.45° per degree of steer) and tips part of the yaw rate onto the roll
// gyro. In a corner the steering angle follows from the yaw rate, the speed
// and the wheelbase, so the filter estimates it and turns the readings back
// into the frame's axes. That covers a steady corner. It cannot see a
// countersteer, bars shaking over rocks, or a bike stopped with the bars
// turned: on the side stand at full lock the lean reads what the bars read.
//
// **A sliding rear breaks the reference, not the gyro.** v·Ω assumes the
// bike points where it is going. When the rear steps out the heading turns
// faster than the path, by the rate the sideslip grows, and v·Ω reads a lean
// the bike never had; the steering estimate, which assumes the rear tracks,
// reads opposite lock as the bars turned into the corner. So every GPS fix
// (`on_fix`) compares the gyro's heading change over the last two fix
// intervals with the course's, which sideslip does not move. When they
// disagree the bike is sliding (or in the air, where v·Ω means nothing
// either): the correction stops, the steering estimate goes back to where it
// was before the window, and the gyro carries the lean alone until the two
// agree again. Up to ~7° of error in a caught slide became ~1-2°.
//
// Axis convention, which the IMU driver is responsible for satisfying. A
// right-handed frame, x forward, y LEFT, z up, on the board as mounted —
// what a right-handed part reads with no axis negated:
//
//   Upright and at rest:      ax = 0,      ay = 0,       az = +1 g
//   Leaning right by φ:       ay = +sin φ, az = +cos φ
//   Nose up by θ:             ax = +sin θ
//   gx > 0 rolling right, gy > 0 pitching nose DOWN, gz > 0 yawing LEFT.
//
// The filter turns those into roll right, nose up and yaw right itself. An
// earlier version of this comment asked for "y right" with the same signs,
// which no real part can give: one axis would have had to be negated, and
// a negated gz would have read a right turn as a left lean.
//
// Check it on the bench with the DIAG IMU screen before trusting a number
// off this filter, and then once moving (bringup §6): a sign error here
// reads as a perfectly smooth, perfectly wrong lean angle.
#pragma once
#include <cstddef>
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

    // The published g vector is low-passed with this time constant (about
    // a 10 Hz corner). Raw, it was whichever single 200 Hz sample came last,
    // and `logic` reads it at 50 Hz for the fused speed: engine vibration
    // (25-170 Hz) folded straight into the speed estimate. The lean filter
    // still works from the raw samples.
    float g_tau_ms = 16.0f;

    // Steering geometry, for the bar-mounted IMU. BRINGUP: both are the
    // nominal CRF250F figures, not measured on this bike. A wheelbase of 0
    // means the IMU is on the frame and nothing is corrected.
    float rake_deg     = 27.0f;
    float wheelbase_ft = 4.63f;
    // Never estimate past the steering lock.
    float max_steer_deg = 40.0f;
    // The estimate is smoothed: it comes from the yaw rate, which carries
    // the bars' own shake.
    float steer_tau_ms = 50.0f;

    // Slide detection. Below this GPS speed the course is too noisy to
    // difference (and v·Ω is barely used).
    float    slide_min_mph = 10.0f;
    // Heading-minus-course rate over the window that counts as a slide. GPS
    // course noise of ±0.5° a fix is ~5 dps over 200 ms; a slide is tens.
    // A false alarm costs only a moment of gyro-only lean, so this is low.
    // BRINGUP: a guess, never seen a real slide; tune from a log.
    float    slide_rate_dps = 20.0f;
    // Held this long after the last fix that saw one: the next fix's verdict
    // covers a window that still has the slide in it.
    uint32_t slide_hold_ms  = 600;
};

struct Sample {
    float    gx_dps = 0.0f;    // about x (forward): > 0 rolling right
    float    gy_dps = 0.0f;    // about y (left):    > 0 nose down
    float    gz_dps = 0.0f;    // about z (up):      > 0 yawing left
    float    ax_g   = 0.0f;
    float    ay_g   = 0.0f;
    float    az_g   = 1.0f;
    float    speed_mph = 0.0f; // road speed; the correction term needs it
    uint32_t t_ms   = 0;
};

class Filter {
public:
    // The board does not sit level on the bars. Without this every lean
    // reading carries a fixed bias. Set from NVS at boot. Taken out as a
    // rotation of all six axes, not by subtracting angles: a dash tilted
    // back towards the rider otherwise mixes yaw into roll.
    void set_mount_offset(float roll_deg, float pitch_deg);

    // The ImuCalibrate command: park upright, bars straight, hold E. Takes
    // the current gravity vector as the new zero. Returns the offsets it
    // stored so the caller can write them to NVS. Bars a few degrees off
    // centre here are a lean error for good, 0.45° per degree.
    void calibrate_level(const Sample& s, float& roll_deg, float& pitch_deg);

    // Zero the gyro bias from a stationary average, in the sensor's axes.
    // Called during boot while the bike is still, and after any calibrate.
    void set_gyro_bias(float gx_dps, float gy_dps, float gz_dps);

    void update(const Sample& s, const Config& c);
    void reset();

    // A new GPS fix: its course (° from north, clockwise), its Doppler speed
    // and when it was taken on the system clock (core::fixtime's stamp, not
    // its arrival). `ok` false is a fix without a usable course, and starts
    // the window over. Call after the samples up to its arrival.
    void on_fix(float course_deg, float speed_mph, uint32_t t_ms, bool ok,
                const Config& c);

    // Whether the last fixes saw the heading leave the path.
    bool  sliding() const { return static_cast<int32_t>(slide_until_ms_ - last_ms_) > 0; }
    // The last window's heading-minus-course rate, > 0 the tail stepping
    // out of a right turn. For DIAG and tuning `slide_rate_dps`.
    float slide_rate_dps() const { return slide_rate_dps_; }

    float lean_deg() const  { return lean_deg_; }
    float pitch_deg() const { return pitch_deg_; }

    // The steering angle the filter is correcting for, > 0 to the right.
    float steer_deg() const { return steer_deg_; }

    // What the accelerometer alone would say. Not used for lean — it is on
    // the DIAG IMU screen precisely so the difference is visible, which is
    // the quickest way to prove the filter is doing something.
    float accel_lean_deg() const { return accel_lean_deg_; }

    // Fills the lean, pitch, yaw-rate and g fields. The jump fields belong
    // to core/jump and are left alone. The yaw rate is about the world's
    // vertical, > 0 turning right; the g vector is in the frame's axes.
    void publish_into(state::Attitude& a) const;

private:
    float lean_deg_        = 0.0f;
    float pitch_deg_       = 0.0f;
    float accel_lean_deg_  = 0.0f;
    float steer_deg_       = 0.0f;
    float bias_gx_ = 0.0f, bias_gy_ = 0.0f, bias_gz_ = 0.0f;

    // Sensor axes to the bars' axes: the mount offset, as a rotation.
    float mount_[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    float lx_ = 0.0f, ly_ = 0.0f, lz_ = 1.0f;     // g, low-passed, published
    float yaw_rate_dps_ = 0.0f;                  // world, > 0 right

    // The gyro's heading, integrated and unwrapped, and a 20 ms history of
    // it and of the steering estimate, long enough to reach back over two
    // fix intervals and the receiver's latency.
    struct Past { uint32_t t_ms; float heading_deg; float steer_deg; };
    static constexpr size_t kPast = 24;
    float    heading_deg_ = 0.0f;
    Past     past_[kPast] = {};
    size_t   past_n_ = 0, past_head_ = 0;
    bool     past_at(uint32_t t_ms, Past& out) const;

    struct Fix { uint32_t t_ms; float course_deg; bool ok; };
    Fix      fixes_[3] = {};
    size_t   fixes_n_ = 0;
    uint32_t slide_until_ms_ = 0;
    float    slide_rate_dps_ = 0.0f;

    uint32_t last_ms_ = 0;
    bool     started_ = false;
};

// Calibrate level's measurement: a second of raw samples from a bike
// standing still, averaged. One sample that moved -- the rider leaning on
// it, a foot on the peg -- and the run is refused rather than storing a
// level that was not one. The mean feeds Filter::calibrate_level (its
// accelerometer) and Filter::set_gyro_bias (its gyro).
class LevelCal {
public:
    static constexpr size_t kSamples = 200;        // 1 s at 200 Hz
    // Still: within this of 1 g, and every gyro axis under this.
    static constexpr float kStillG   = 0.05f;
    static constexpr float kStillDps = 5.0f;

    enum class Result : uint8_t { Idle, Pending, Done, Moved };

    void  start() { n_ = 0; active_ = true; sum_ = Sum{}; }
    bool  active() const { return active_; }
    // Raw, as the driver gives it: no bias taken off, no mount turned out.
    Result feed(const Sample& raw);
    // The average, once feed() has said Done.
    const Sample& mean() const { return mean_; }

private:
    struct Sum { double ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0; };
    Sum    sum_;
    Sample mean_;
    size_t n_ = 0;
    bool   active_ = false;
};

// When a sample in a FIFO batch was actually taken.
//
// A batch is read all at once, so the obvious thing — stamping every sample
// with the read time — hands the filter a run of identical timestamps. It
// drops all but the first of those (dt == 0 is not integrable), which turns
// a 200 Hz stream into the poll rate and aliases everything in between.
// On a dirt bike, what lives between those rates is vibration.
//
// So the batch is back-dated: the newest sample is at `now_ms` and the ones
// before it step back by the sample period. `index` is 0 for the oldest.
//
// This lives in core/ rather than in the driver because it is arithmetic
// and the driver is not host-testable — and getting it wrong is silent.
inline uint32_t sample_time_ms(uint32_t now_ms, size_t index, size_t count,
                               uint32_t period_ms) {
    if (count == 0) return now_ms;
    const size_t back = (count - 1) - (index < count ? index : count - 1);
    return now_ms - static_cast<uint32_t>(back) * period_ms;
}

}  // namespace core::attitude
