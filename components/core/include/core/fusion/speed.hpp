// Fused road speed — stage 1 of sensor fusion (§ fusion, docs/fusion.md).
//
// A two-state Kalman filter: speed, and the forward accelerometer's bias.
// The IMU predicts at the logic rate; the wheels and GPS Doppler correct.
// Each sensor covers another's weakness:
//
//   - the wheels are exact and instant, until the rear spins under power
//     or the front locks under braking;
//   - GPS Doppler never spins or locks, but arrives at 10 Hz, late, and
//     is nonsense with a poor fix;
//   - the accelerometer sees a change of speed at once, but integrated on
//     its own it drifts in seconds — which is what the bias state is for.
//
// A correction that disagrees with the prediction by more than `gate_sigma`
// standard deviations is rejected rather than followed: that is how a
// spinning rear wheel or a GPS glitch is kept out. A source rejected for
// `suspect_ms` straight is reported as suspect, so DIAG can say which.
// If everything is being rejected at once the filter has lost the plot,
// and it resets onto the most trusted source rather than arguing with it.
//
// Pure logic. Time comes in as a parameter, like everything in core/.
#pragma once
#include <cstdint>

namespace core::fusion {

// 1 g of forward acceleration, in mph per second.
inline constexpr float kMphPerSecPerG = 21.937f;

struct SpeedConfig {
    // Process noise. How far the truth may wander from "speed changed by
    // what the accelerometer said" in one second, as a variance: vibration,
    // pitch on a hill reading as acceleration, the IMU not quite square.
    float q_speed      = 4.0f;      // (mph/s)^2 per s
    float q_bias       = 1.0e-4f;   // g^2 per s: the bias wanders slowly
    // Measurement noise, as a standard deviation.
    float r_wheel_mph  = 0.6f;
    float r_gps_mph    = 1.2f;      // at HDOP 1; scaled up with HDOP
    float gate_sigma   = 4.0f;
    // The wheels disagreeing by more than this means one of them is
    // spinning or locked. The gate alone does not catch a spin that builds
    // up gently, a fraction of a mph per tick, so when they split only the
    // wheel nearer the filter's own estimate (IMU plus GPS) is used.
    float wheel_split_mph = 2.0f;
    // GPS is the referee for the wheels. While an accepted fix is younger
    // than `referee_ms`, a wheel further than this from it (or a quarter of
    // it, if more) is not believed, however smoothly it got there: that is
    // a dead sensor reading 0 at 40 mph, or a spin with no front wheel to
    // compare against. Only an accepted fix referees, so a GPS glitch
    // cannot throw the wheels out with it.
    float    wheel_gps_mph = 4.0f;
    uint32_t referee_ms    = 1000;
    uint32_t suspect_ms = 1500;     // rejected this long straight -> suspect
    // How long the filter will coast on the accelerometer alone before it
    // stops claiming to know the speed.
    uint32_t coast_ms   = 2000;
    // Longer than this between steps (a stall, a breakpoint) and the filter
    // restarts rather than integrating one enormous dt.
    uint32_t max_dt_ms  = 500;
};

struct SpeedInput {
    uint32_t t_ms = 0;
    float accel_g = 0.0f;      bool accel_ok = false;   // forward +
    float front_mph = 0.0f;    bool front_ok = false;
    float rear_mph = 0.0f;     bool rear_ok = false;
    float gps_mph = 0.0f;      bool gps_ok = false;
    // True only on the tick a new fix arrived. The same Doppler figure fed
    // in five times would be believed five times as hard.
    bool  gps_new = false;
    float hdop = 1.0f;
};

// Which sources the last step used, as bits, and which are suspect.
enum Source : uint8_t {
    kSrcAccel = 1u << 0,
    kSrcFront = 1u << 1,
    kSrcRear  = 1u << 2,
    kSrcGps   = 1u << 3,
};

struct SpeedOutput {
    float   mph = 0.0f;
    float   sigma_mph = 0.0f;  // one standard deviation
    bool    valid = false;
    uint8_t used = 0;          // Source bits accepted this step
    uint8_t rejected = 0;      // Source bits gated out this step
    uint8_t suspect = 0;       // Source bits rejected for suspect_ms straight
    float   bias_g = 0.0f;
    uint16_t resets = 0;
};

class SpeedFilter {
public:
    SpeedOutput step(const SpeedInput& in, const SpeedConfig& cfg = {});
    const SpeedOutput& last() const { return out_; }
    void reset() { *this = SpeedFilter{}; }

private:
    // Returns true if accepted.
    bool correct(float z, float r_sigma, float gate_sigma);
    void start(float v, float var);
    void track(uint8_t bit, bool accepted, bool offered, uint32_t now,
               const SpeedConfig& cfg);

    bool     started_ = false;
    float    v_ = 0.0f;                   // mph
    float    b_ = 0.0f;                   // g
    float    p_[2][2] = {{0, 0}, {0, 0}};
    uint32_t last_t_ = 0;
    uint32_t last_fix_t_ = 0;             // last accepted correction
    float    ref_mph_ = 0.0f;             // last accepted GPS speed
    uint32_t ref_t_ = 0;
    bool     have_ref_ = false;
    uint32_t reject_since_[4] = {0, 0, 0, 0};
    bool     rejecting_[4] = {false, false, false, false};
    SpeedOutput out_;
};

// Is each wheel speed sensor alive? Decided from the speed filter, so it
// only ever knows anything while the bike is moving and something else
// (GPS, the other wheel) says how fast.
//
//   Unknown  nothing proven yet: parked since key-on, or never cross-checked
//   Ok       read a plausible speed while moving, so it is connected
//   Dead     read zero, and was rejected, for `dead_ms` straight while the
//            filter says moving: unplugged, cut, or a sensor gap gone too
//            wide
//
// A spinning or locked wheel is rejected too, but a spin reads high, and a
// lock or a skid reads zero for a second or three, not five. So a burnout,
// a wheelie or a long rear slide into a corner does not mark a good sensor
// faulty. A Dead wheel goes back to Ok
// the first time it reads a plausible speed again: a loose connector that
// comes back is not stuck as a fault.
enum class Wheel : uint8_t { Unknown = 0, Ok, Dead };

struct WheelCheckConfig {
    float moving_mph = 5.0f;   // the filter's speed, to call the bike moving
    float zero_mph   = 1.0f;   // a wheel below this is reading nothing
    uint32_t dead_ms = 5000;   // longer than any skid
};

class WheelCheck {
public:
    void update(const SpeedInput& in, const SpeedOutput& out,
                const WheelCheckConfig& cfg = {});
    Wheel front() const { return f_.verdict; }
    Wheel rear() const { return r_.verdict; }

private:
    struct One {
        Wheel    verdict = Wheel::Unknown;
        bool     zeroing = false;     // reading zero at speed right now
        uint32_t zero_since = 0;
    };
    static void judge(One& w, bool offered, float wheel_mph, uint8_t bit,
                      const SpeedOutput& out, uint32_t now, const WheelCheckConfig& cfg);
    One f_, r_;
};

}  // namespace core::fusion
