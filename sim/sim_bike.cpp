#include "sim_bike.hpp"
#include "core/geo/geo.hpp"
#include "core/shift/shift.hpp"
#include <cmath>

namespace sim {
namespace {

constexpr double kPi = 3.14159265358979;
constexpr float  kG_ftps2 = 32.174f;
constexpr float  kFtPerSPerMph = 1.46666667f;

// Degrees per foot at 42N, matching the flat frame core/geo uses so the
// simulated fixes and the finish-line geometry agree exactly.
const double kFtLat = 1.0 / 364000.0;
const double kFtLon = 1.0 / (364000.0 * std::cos(42.0 * kPi / 180.0));

// A rounded rectangle: two 900 ft straights joined by 300 ft radius turns.
constexpr float kStraight_ft = 900.0f;
constexpr float kRadius_ft   = 300.0f;
const float kTurn_ft = static_cast<float>(kPi * kRadius_ft);
const float kLap_ft  = 2.0f * kStraight_ft + 2.0f * kTurn_ft;

// Position and heading at distance `s` round the circuit.
void at(float s, float& x_ft, float& y_ft, float& heading_deg,
        float& curvature) {
    while (s < 0.0f) s += kLap_ft;
    s = std::fmod(s, kLap_ft);

    if (s < kStraight_ft) {                       // north up the back straight
        x_ft = 0.0f;
        y_ft = s;
        heading_deg = 0.0f;
        curvature = 0.0f;
    } else if (s < kStraight_ft + kTurn_ft) {     // right-hand turn
        const float a = (s - kStraight_ft) / kRadius_ft;
        x_ft = kRadius_ft * (1.0f - std::cos(a));
        y_ft = kStraight_ft + kRadius_ft * std::sin(a);
        heading_deg = static_cast<float>(a * 180.0 / kPi);
        curvature = 1.0f / kRadius_ft;
    } else if (s < 2.0f * kStraight_ft + kTurn_ft) {  // south, down the front
        const float d = s - kStraight_ft - kTurn_ft;
        x_ft = 2.0f * kRadius_ft;
        y_ft = kStraight_ft - d;
        heading_deg = 180.0f;
        curvature = 0.0f;
    } else {                                      // the other turn
        const float a = (s - 2.0f * kStraight_ft - kTurn_ft) / kRadius_ft;
        x_ft = kRadius_ft * (1.0f + std::cos(a));
        y_ft = -kRadius_ft * std::sin(a);
        heading_deg = static_cast<float>(180.0 + a * 180.0 / kPi);
        curvature = 1.0f / kRadius_ft;
    }
}

}  // namespace

void Bike::reset() {
    f_ = 0.0f;
    lat_ = 42.500000;
    lon_ = -83.100000;
    heading_deg_ = 0.0f;
    lean_deg_ = 0.0f;
}

void Bike::step(core::state::Slots& slots, const Controls& c, uint32_t now_ms,
                uint32_t dt_ms) {
    const float dt = static_cast<float>(dt_ms) / 1000.0f;

    float yaw_rate_dps = 0.0f;

    if (c.auto_ride) {
        const float v_ftps = c.speed_mph * kFtPerSPerMph;
        f_ += (v_ftps * dt) / kLap_ft;
        while (f_ >= 1.0f) f_ -= 1.0f;

        float x, y, hdg, curv;
        at(f_ * kLap_ft, x, y, hdg, curv);
        heading_deg_ = hdg;

        // Lean that holds this speed through this corner — the same
        // coordinated-turn relation core/attitude uses as its reference, so
        // the filter has something physically consistent to track.
        const float omega_rps = curv * v_ftps;                 // rad/s
        yaw_rate_dps = static_cast<float>(omega_rps * 180.0 / kPi);
        const float corner_deg = static_cast<float>(
            std::atan2(v_ftps * omega_rps, kG_ftps2) * 180.0 / kPi);

        // The slider adds to the corner rather than being ignored by it.
        // Overwriting lean here meant that ticking "ride the circuit" — which
        // is what you do to make the lap timer and the maps work — silently
        // disconnected the lean slider, and the gauge looked broken when it
        // was the simulated bike that had stopped listening.
        lean_deg_ = corner_deg + c.lean_deg;
        if (lean_deg_ < -60.0f) lean_deg_ = -60.0f;
        if (lean_deg_ > 60.0f) lean_deg_ = 60.0f;

        lat_ = 42.500000 + y * kFtLat;
        lon_ = -83.100000 + x * kFtLon;
    } else {
        // Off the circuit the bike still moves: a receiver at 42 mph is not
        // a receiver sitting at the start line. Without this, every fix was
        // the same coordinate forever unless "ride the circuit" happened to
        // be ticked, so course, the maps and the trip meters read as broken
        // when it was the simulated bike that was parked.
        lean_deg_ = c.lean_deg;
        heading_deg_ = 0.0f;
        lat_ += static_cast<double>(c.speed_mph * kFtPerSPerMph * dt) * kFtLat;
    }

    core::state::Engine e;
    e.shift_mask = c.shift_mask;
    e.shift_flash = c.shift_flash;
    e.gear = c.gear;
    e.rpm = c.rpm;
    e.neutral = c.neutral;
    e.coolant_c = c.coolant_c;
    e.tps_pct = c.tps_pct;
    e.ecu_online = c.ecu_online;
    slots.engine.publish(e);

    core::state::Motion m;
    m.gps_mph = c.speed_mph;
    m.front_mph = c.speed_mph;
    m.rear_mph = c.speed_mph;
    m.best_mph = c.speed_mph;
    m.accel_g = 0.0f;
    slots.motion.publish(m);

    // Published directly rather than run through core/attitude: the filter
    // is fed by a 200 Hz gyro stream this simulator does not synthesise, and
    // it already has its own tests. Everything the screens do with these
    // numbers is real.
    core::state::Attitude a;
    a.lean_deg = lean_deg_;
    a.pitch_deg = c.pitch_deg;
    a.yaw_rate_dps = yaw_rate_dps;
    const float lean_rad = lean_deg_ * static_cast<float>(kPi) / 180.0f;
    a.g_lat = std::sin(lean_rad);
    a.g_vert = std::cos(lean_rad);
    a.g_lon = 0.0f;
    slots.attitude.publish(a);

    // A receiver, not a checkbox. Four satellites is the arithmetic minimum
    // for a 3D fix, and accuracy follows how many are up rather than being a
    // constant — HDOP was pinned at 0.8, so the accuracy readout showed the
    // same 13 ft whatever the satellite slider was doing, which is the whole
    // GPS block on the speed screen sitting there inert.
    const bool enough = c.sats >= 4;
    core::state::Gnss g;
    g.fix = c.gps_fix && enough;
    g.sats = static_cast<uint8_t>(c.sats);
    g.quality = g.fix ? 1 : 0;
    g.lat = lat_;
    g.lon = lon_;
    g.alt_m = 265.0f;
    // Roughly inverse: about 0.8 with twelve in view, past 2 down at the
    // edge of a fix. Close enough that the readout moves for the right
    // reason; a real HDOP is geometry, not a count.
    g.hdop = enough ? 9.6f / static_cast<float>(c.sats) : 99.9f;
    g.course_deg = heading_deg_;
    g.speed_mph = c.speed_mph;
    g.utc_ms = now_ms;
    slots.gnss.publish(g);

    core::state::Elec el;
    el.vbat = c.vbat;
    el.v5 = 5.02f;
    el.v3v3 = 3.31f;
    slots.elec.publish(el);
}

}  // namespace sim
