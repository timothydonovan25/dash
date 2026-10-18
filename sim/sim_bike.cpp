#include "sim_bike.hpp"
#include "core/geo/geo.hpp"
#include "core/shift/shift.hpp"
#include <cmath>
#include <cstring>

namespace sim {
namespace {

constexpr double kPi = 3.14159265358979;
constexpr float  kG_ftps2 = 32.174f;
constexpr float  kFtPerSPerMph = 1.46666667f;

// How fast the bike can actually change speed: 0.8 g on the throttle and
// 1.1 g on the brakes, in mph per second. The speed slider is where the
// rider wants to be; the bike gets there at these rates.
constexpr float kAccelMphPerS = 17.5f;
constexpr float kBrakeMphPerS = 24.0f;

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

// A slip event's shape, in percent against road speed, `t` ms in; nothing
// once it is over. A spin builds over 150 ms to +45 %, holds, and dies
// away by 900 ms: a rear stepping out on a throttle stab. A lock is the
// wheel stopping within 100 ms and staying stopped: -100 % is 0 mph.
bool slip_shape(sim::Bike::Slip kind, uint32_t t, float& pct) {
    using Slip = sim::Bike::Slip;
    if (kind == Slip::Spin) {
        if (t >= 900) return false;
        if (t < 150)       pct = 45.0f * static_cast<float>(t) / 150.0f;
        else if (t < 600)  pct = 45.0f;
        else               pct = 45.0f * static_cast<float>(900 - t) / 300.0f;
        return true;
    }
    const uint32_t hold = kind == Slip::FrontLock ? 500 : 900;
    if (t >= hold) return false;
    pct = t < 100 ? -100.0f * static_cast<float>(t) / 100.0f : -100.0f;
    return true;
}

template <size_t N>
void copy_into(char (&dst)[N], const char* src) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < N; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

}  // namespace

void Bike::reset() {
    f_ = 0.0f;
    speed_mph_ = 0.0f;
    lat_ = 42.500000;
    lon_ = -83.100000;
    heading_deg_ = 0.0f;
    lean_deg_ = 0.0f;
    media_position_ms_ = 0;
    media_title_prev_[0] = '\0';
}

void Bike::trigger_jump(uint32_t now_ms, uint32_t airtime_ms) {
    air_until_ms_ = now_ms + airtime_ms;
    // The landing: a couple of hundred milliseconds above the detector's
    // threshold, which is long enough for it to find the peak inside its
    // impact window rather than sampling the trigger.
    spike_until_ms_ = air_until_ms_ + 200;
}

void Bike::trigger_slip(Slip kind, uint32_t now_ms) {
    slip_at_ms_[static_cast<uint8_t>(kind)] = now_ms ? now_ms : 1;
}

void Bike::trigger_crank(uint32_t now_ms, float rest_v) {
    crank_at_ms_ = now_ms ? now_ms : 1;
    crank_rest_v_ = rest_v;
}

void Bike::step(core::state::Slots& slots, const Controls& c, uint32_t now_ms,
                uint32_t dt_ms) {
    const float dt = static_cast<float>(dt_ms) / 1000.0f;

    // Road speed follows the slider at what a bike can do, not in one step.
    // In one step, all three speeds jumped together while fused road speed
    // (logic's, which gates a jump that size as implausible) caught up over
    // a second or so, and the slip bar called both wheels locked, or both
    // spinning, against it. Real 1 g stops do not do that; a slider did.
    const float want = c.down ? 0.0f : c.speed_mph;
    if (want > speed_mph_) {
        speed_mph_ = std::fmin(want, speed_mph_ + kAccelMphPerS * dt);
    } else {
        speed_mph_ = std::fmax(want, speed_mph_ - kBrakeMphPerS * dt);
    }
    const float v_mph = speed_mph_;

    // Longitudinal g, from how fast the speed is changing. Low passed: the
    // ramp above is a step in acceleration, and the ball should not snap.
    const float dv_ftps = (v_mph - prev_speed_mph_) * kFtPerSPerMph;
    prev_speed_mph_ = v_mph;
    const float raw_g = dt > 0.0f ? (dv_ftps / dt) / kG_ftps2 : 0.0f;
    accel_g_ += 0.15f * (raw_g - accel_g_);
    if (accel_g_ < -1.2f) accel_g_ = -1.2f;
    if (accel_g_ >  1.2f) accel_g_ =  1.2f;

    float yaw_rate_dps = 0.0f;

    if (c.auto_ride) {
        const float v_ftps = v_mph * kFtPerSPerMph;
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
        lat_ += static_cast<double>(v_mph * kFtPerSPerMph * dt) * kFtLat;
    }

    // The starter, while it turns: 60 ms of inrush, then what the battery
    // holds under the load. A healthy one at 12.6 V holds about 10.2 V; one
    // that is partly flat has less in it and sags further (at 12.3 V it is
    // under the 9.6 V load-test line), which is what core::battery reads.
    rpm_ = c.rpm;
    vbat_ = c.vbat;
    if (crank_at_ms_ != 0) {
        const uint32_t t = now_ms - crank_at_ms_;
        if (t < 1000) {
            rpm_ = t < 80 ? 0.0f : 280.0f;
            const float flat = crank_rest_v_ < 12.6f ? 12.6f - crank_rest_v_ : 0.0f;
            const float sag = 2.4f + 4.0f * flat;
            vbat_ = crank_rest_v_ - (t < 60 ? sag + 1.2f : sag);
        } else {
            crank_at_ms_ = 0;
        }
    }

    core::state::Engine e;
    e.shift_mask = c.shift_mask;
    e.shift_flash = c.shift_flash;
    e.gear = c.gear;
    e.rpm = rpm_;
    e.neutral = c.neutral;
    e.tps_pct = c.tps_pct;
    e.ecu_online = c.ecu_online;
    slots.engine.publish(e);

    core::state::Motion m;
    m.gps_mph = v_mph;
    // The sliders, with any slip event running played over them: the
    // event wins while it lasts, whichever way it is further from zero.
    float front_pct = c.front_slip_pct;
    float rear_pct = c.rear_slip_pct;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Slip::Count); ++i) {
        if (slip_at_ms_[i] == 0) continue;
        const auto kind = static_cast<Slip>(i);
        float pct = 0.0f;
        if (!slip_shape(kind, now_ms - slip_at_ms_[i], pct)) {
            slip_at_ms_[i] = 0;
            continue;
        }
        float& wheel = kind == Slip::FrontLock ? front_pct : rear_pct;
        if (std::fabs(pct) > std::fabs(wheel)) wheel = pct;
    }
    m.front_mph = v_mph * (1.0f + front_pct / 100.0f);
    m.rear_mph = v_mph * (1.0f + rear_pct / 100.0f);
    if (m.front_mph < 0.0f || c.front_dead) m.front_mph = 0.0f;
    if (m.rear_mph < 0.0f || c.rear_dead) m.rear_mph = 0.0f;
    m.best_mph = v_mph;
    m.accel_g = accel_g_;
    slots.motion.publish(m);

    // Published directly rather than run through core/attitude: the filter
    // is fed by a 200 Hz gyro stream this simulator does not synthesise, and
    // it already has its own tests. Everything the screens do with these
    // numbers is real.
    core::state::Attitude a;
    // Over on its side, whichever side the slider last leaned it.
    a.lean_deg = c.down ? (c.lean_deg < 0.0f ? -88.0f : 88.0f) : lean_deg_;
    a.imu_temp_c = c.dash_temp_c - 3.0f;   // the IMU runs a little cooler than the P4
    a.pitch_deg = c.pitch_deg;
    a.yaw_rate_dps = yaw_rate_dps;
    const float lean_rad = a.lean_deg * static_cast<float>(kPi) / 180.0f;
    a.g_lat = std::sin(lean_rad);
    a.g_vert = std::cos(lean_rad);
    a.g_lon = accel_g_;

    // What the accelerometer reads, fed to the firmware's own detector
    // rather than to a flag that says "jumping". In free fall the magnitude
    // is zero -- not one, not "down" -- which is the whole basis of core/jump,
    // so the simulator has to reproduce that and not shortcut it.
    float mag_g;
    if (now_ms < air_until_ms_) {
        mag_g = 0.0f;
    } else if (now_ms < spike_until_ms_) {
        mag_g = 4.2f;
    } else {
        mag_g = std::sqrt(a.g_lat * a.g_lat + a.g_vert * a.g_vert +
                          a.g_lon * a.g_lon);
    }
    jump_.update(mag_g, now_ms, core::jump::Config{});
    jump_.publish_into(a);
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
    g.speed_mph = v_mph;
    g.utc_ms = c.utc_ms;
    g.year = c.year;
    g.month = c.month;
    g.day = c.day;
    g.day_of_year = c.day_of_year;
    g.rx_ms = now_ms;            // a new fix every step
    slots.gnss.publish(g);

    // What `ble` will publish once it exists, taken meanwhile from whatever
    // is playing on this machine. `updated_ms` stays zero when there is no
    // title, which is the gate every screen reads: no media, not a song with
    // no name.
    core::state::Media md;
    if (c.media_title[0] != '\0') {
        copy_into(md.title,  c.media_title);
        copy_into(md.album,  c.media_album);
        copy_into(md.artist, c.media_artist);
        copy_into(md.lyric,  c.media_lyric);
        md.state = c.media_state;
        md.updated_ms = now_ms;
        md.duration_ms = c.media_duration_ms;

        if (c.media_position_known) {
            md.position_ms = c.media_position_ms;
            md.position_at_ms = c.media_position_at_ms;
        } else {
            // No player to ask, so this plays the phone's part itself: a
            // new title restarts the clock, and wrapping at the duration,
            // if one was given, keeps the ring worth looking at for longer
            // than one track.
            if (std::strcmp(c.media_title, media_title_prev_) != 0) {
                media_position_ms_ = 0;
                copy_into(media_title_prev_, c.media_title);
            } else if (c.media_state == 1) {
                media_position_ms_ += dt_ms;
                if (c.media_duration_ms > 0 && media_position_ms_ >= c.media_duration_ms) {
                    media_position_ms_ %= c.media_duration_ms;
                }
            }
            md.position_ms = media_position_ms_;
            md.position_at_ms = now_ms;
        }
    }
    slots.media.publish(md);

    core::state::Elec el;
    el.vbat = vbat_;
    slots.elec.publish(el);

    battery_.update(vbat_, rpm_, now_ms, core::battery::Config{});
    const core::battery::Report& br = battery_.report();
    core::state::Battery b;
    b.rest_v = br.rest_v;
    b.crank_min_v = br.crank_min_v;
    b.charge_v = br.charge_v;
    b.verdict = static_cast<uint8_t>(br.verdict);
    slots.battery.publish(b);
}

}  // namespace sim
