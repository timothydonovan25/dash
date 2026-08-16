#include "core/jump/jump.hpp"

namespace core::jump {

void Detector::reset() {
    airborne_ = false;
    falling_ = false;
    impacting_ = false;
    suspended_ = false;
    airtime_ms_ = 0;
    peak_g_ = 0.0f;
}

Update Detector::update(float mag_g, uint32_t t_ms, const Config& c) {
    Update out;

    // An IMU that has stopped answering reads a steady zero, which is
    // indistinguishable from free fall. After one implausible airtime we
    // stop believing it until it produces something that is not free fall
    // again — otherwise the dash sits there declaring the bike airborne,
    // resetting every eight seconds, for the rest of the ride.
    if (suspended_) {
        if (mag_g >= c.free_fall_g) suspended_ = false;
        return out;
    }

    // Down, but still inside the window where the suspension is loading up.
    if (impacting_) {
        if (mag_g > peak_g_) peak_g_ = mag_g;
        if (t_ms - impact_start_ >= c.impact_window_ms) {
            impacting_ = false;
            last_landing_g_ = peak_g_;
            out.event = Event::Landed;
            out.airtime_ms = landed_airtime_ms_;
            out.landing_g = peak_g_;
        }
        return out;
    }

    if (!airborne_) {
        if (mag_g < c.free_fall_g) {
            if (!falling_) {
                falling_ = true;
                fall_start_ = t_ms;
            } else if (t_ms - fall_start_ >= c.min_free_fall_ms) {
                // Airtime is counted from when the wheels left, not from
                // when we became sure — the qualifying window is evidence,
                // not part of the jump.
                airborne_ = true;
                falling_ = false;
                airtime_ms_ = t_ms - fall_start_;
                peak_g_ = 0.0f;
                out.event = Event::TookOff;
            }
        } else {
            falling_ = false;      // a whoop, not a jump
        }
        return out;
    }

    // Airborne.
    airtime_ms_ = t_ms - fall_start_;
    if (mag_g > peak_g_) peak_g_ = mag_g;

    if (mag_g >= c.landing_g) {
        // Touchdown. Airtime is settled here — the impact window that
        // follows is only about how hard, not how long.
        airborne_ = false;
        falling_ = false;
        impacting_ = true;
        impact_start_ = t_ms;
        landed_airtime_ms_ = airtime_ms_;
        if (airtime_ms_ > best_ms_) best_ms_ = airtime_ms_;
        return out;
    }

    if (airtime_ms_ > c.max_airtime_ms) {
        // Nobody jumps a CRF250F for eight seconds. Either the IMU stopped
        // answering or it is reading nonsense; either way, silently drop the
        // jump rather than publish a record airtime the rider never had.
        reset();
        suspended_ = true;
    }
    return out;
}

void Detector::publish_into(state::Attitude& a) const {
    a.airborne = airborne_;
    a.airtime_ms = airtime_ms_;
    a.last_land_g = last_landing_g_;
}

}  // namespace core::jump
