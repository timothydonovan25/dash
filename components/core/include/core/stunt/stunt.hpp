// Wheelies and stoppies, detected from the IMU and scored like a video game.
//
// Detection. A wheelie is the bike pitched nose-up against the ground it is
// riding on. The IMU's pitch is against level, so a hill reads the same as a
// wheelie to it; what tells them apart is which way the bike is *moving*. On
// a hill the bike travels along its own nose, climbing; in a wheelie it
// points up and goes straight on. So the ground's angle is the angle of the
// bike's path, asin(climb rate / speed), and the climb rate comes from a
// small inertial-plus-GPS filter: the accelerometer rotated into the world
// frame and integrated for the fast part, pulled toward GPS altitude for the
// slow part (the baro-inertial blend aircraft use, with GPS as the baro).
// What is left, pitch minus path, is the lift:
//
//     lift = pitch − asin(climb / speed)
//
// Lift up past `start_deg` for `confirm_ms` is a wheelie, down past it a
// stoppie; it ends when the lift comes back under `end_deg`. Airborne is a
// jump (core::jump), not a trick, and below `min_mph` is not riding.
//
// Length is both time and road distance up, the distance integrated from
// the fused speed over the same passes that score points.
//
// Scoring. Points run up while the trick lasts, faster the higher it is,
// faster still at speed, and half again in the balance-point band. A trick
// shorter than `min_scored_ms` is a twitch, not a trick, and scores nothing.
// After it ends the front wheel's landing is watched for `landing_window_ms`:
// a soft one (peak vertical g under `smooth_g`) is worth 20 % more, a slam
// (over `slam_g`) 30 % less. Tricks inside kComboWindowMs of each other
// chain into a combo, each worth 0.5x more than the last, up to 4x. Nose past
// `loop_deg` is a loop-out: no points, and the combo is gone.
//
// Pure logic. `logic` feeds it every scheduled pass from the Attitude, Fused
// and Gnss slots and publishes the Stunt slot.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::stunt {

inline constexpr uint32_t kComboWindowMs = 4000;

enum Kind : uint8_t { None = 0, Wheelie = 1, Stoppie = 2, Looped = 3 };
enum Landing : uint8_t { Plain = 0, Smooth = 1, Slammed = 2 };

struct Config {
    float    start_deg   = 20.0f;
    float    end_deg     = 4.0f;
    uint32_t confirm_ms  = 400;
    uint32_t end_hold_ms = 120;     // under end_deg this long to be over
    float    min_mph     = 3.0f;
    uint32_t min_scored_ms = 500;
    float    loop_deg    = 70.0f;
    float    balance_lo_deg = 30.0f;
    float    balance_hi_deg = 50.0f;
    float    wheelie_pts_per_s = 100.0f;
    float    stoppie_pts_per_s = 150.0f;   // harder, shorter
    uint32_t landing_window_ms = 300;
    float    smooth_g    = 1.6f;
    float    slam_g      = 2.5f;
    // The climb filter: how hard GPS altitude pulls (natural frequency,
    // rad/s; about a 1.3 s time constant), and how fast an unchecked climb
    // rate leaks away with no fix.
    float    climb_omega = 0.8f;
    float    climb_leak_s = 5.0f;
};

struct Input {
    uint32_t t_ms = 0;
    // Attitude: the bike's pitch (+ nose up) and lean (+ right) against
    // level, and the low-passed specific force in the bike's frame, g.
    float    pitch_deg = 0.0f;
    float    lean_deg = 0.0f;
    float    g_lon = 0.0f;
    float    g_lat = 0.0f;
    float    g_vert = 1.0f;
    bool     airborne = false;
    bool     imu_ok = false;
    // Road speed (core::fusion).
    float    mph = 0.0f;
    bool     speed_valid = false;
    // GPS altitude, when there is a fix.
    bool     gps_fix = false;
    float    gps_alt_m = 0.0f;
};

// The rank of a finished trick, for the toast: "WHEELIE", "SICK", ...
const char* grade_name(uint8_t kind, uint8_t grade);
const char* landing_name(uint8_t landing);

class Scorer {
public:
    void update(const Input& in, const Config& c);
    // STUNT's E: a new session score. Records go with it.
    void reset_session();
    state::Stunt publish() const;

    // The ground's angle under the bike and the lift over it, for tests
    // and DIAG.
    float ground_deg() const { return ground_deg_; }
    float lift_deg() const { return lift_deg_; }

private:
    enum class Phase : uint8_t { Idle, Candidate, Active, Landing };

    void climb_update(const Input& in, float dt, const Config& c);
    void end_trick(uint32_t end_ms, const Config& c);
    void finalize(const Config& c);
    float rate(float lift_abs, float mph, bool balance, const Config& c) const;

    // The climb filter.
    bool  climb_init_ = false;
    float h_m_ = 0.0f;
    float vz_mps_ = 0.0f;
    float ground_deg_ = 0.0f;
    float lift_deg_ = 0.0f;

    bool     have_t_ = false;
    uint32_t last_t_ = 0;

    Phase    phase_ = Phase::Idle;
    uint8_t  kind_ = None;
    uint32_t start_ms_ = 0;
    uint32_t below_since_ = 0;
    bool     below_ = false;
    float    points_ = 0.0f;
    float    ft_ = 0.0f;           // road distance while up, from fused speed
    float    peak_deg_ = 0.0f;
    bool     balance_ = false;
    uint32_t live_ms_ = 0;
    // A trick that has ended, waiting out its landing.
    uint32_t end_ms_ = 0;
    float    land_peak_g_ = 0.0f;

    // Combo and session.
    uint8_t  combo_ = 0;
    uint32_t combo_end_ms_ = 0;    // when the last trick of the combo ended
    bool     combo_live_ = false;
    uint32_t now_ms_ = 0;

    state::Stunt out_{};
};

}  // namespace core::stunt
