// The ride so far, in the numbers a rider asks for at the end of it: how far,
// how long, how fast, the longest jump. Pure logic, fed by `logic` every
// pass, published as the Session slot for TRACK -> SESSION.
//
// A session is a power-on. The key going off at lunch starts a new one,
// which is what the lap timer and the session best already do.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::session {

struct Input {
    uint32_t t_ms = 0;
    float    speed_mph = 0.0f;     // road speed, the trip computer's
    bool     engine_on = false;
    // The IMU's jumps (core::jump, via the Attitude slot): `land_ms` moves
    // once per landing and the other two describe that one.
    bool     imu_ok = false;
    uint32_t land_ms = 0;
    uint32_t airtime_ms = 0;
    float    land_g = 0.0f;
};

// Above this the bike is being ridden: walking it through the paddock is
// not riding time. The lap timer's own arming speed is 10.
inline constexpr float kMovingMph = 3.0f;

class Tracker {
public:
    void update(const Input& in);
    state::Session publish() const;

private:
    // Doubles for the same reason as core::trip: a float of seconds stops
    // counting single ticks after a few hours.
    double   dist_mi_ = 0.0;
    double   moving_s_ = 0.0;
    double   engine_s_ = 0.0;
    float    top_mph_ = 0.0f;
    uint32_t best_air_ms_ = 0;
    float    hardest_land_g_ = 0.0f;
    uint16_t jumps_ = 0;
    uint32_t seen_land_ms_ = 0;
    uint32_t last_ms_ = 0;
    bool     started_ = false;
};

}  // namespace core::session
