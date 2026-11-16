#include "core/alert/alert.hpp"
#include "core/hr/hr.hpp"
#include <cmath>

namespace core::alert {

using state::Dev;
using state::DevState;

const char* text(Kind k) {
    switch (k) {
        case Kind::NoPulse:     return "NO PULSE";
        case Kind::BikeDown:    return "BIKE DOWN";
        case Kind::BatteryHigh: return "OVERCHARGING";
        case Kind::BatteryLow:  return "NOT CHARGING";
        case Kind::FrontWheel:  return "FRONT SPEED SENSOR";
        case Kind::RearWheel:   return "REAR SPEED SENSOR";
        case Kind::GpsLost:     return "GPS LOST";
        case Kind::SdCard:      return "LOG STOPPED";
        case Kind::ShiftLamps:  return "SHIFT LIGHTS OUT";
        case Kind::Imu:         return "IMU FAULT";
        case Kind::Service:     return "SERVICE DUE";
        default:                return "";
    }
}

uint32_t Alerter::onset_ms(Kind k) {
    switch (k) {
        case Kind::NoPulse:     return hr::kNoPulseMs;
        case Kind::BikeDown:    return 2000;   // a wheelie recovery is not a crash
        case Kind::BatteryHigh: return 3000;
        case Kind::BatteryLow:  return 5000;   // cranking and lamps sag it briefly
        case Kind::GpsLost:    return 5000;   // a bridge, a tunnel
        // The health tracker already waited before declaring these.
        default:                return 500;
    }
}

bool Alerter::condition(Kind k, const state::Frame& f) const {
    // vbat is 0 when the reading failed, and below 8 V it is USB, not a bike.
    const bool on_bike = f.elec.vbat > 8.0f;
    switch (k) {
        case Kind::NoPulse:
            return hr::no_pulse(f.heart, f.t_ms);
        case Kind::BikeDown:
            // Retired (2.24.0): a crash is core::crash's countdown, shown only
            // with Crash SOS on; a tip-over alone says nothing. The slot stays
            // so the kinds after it keep their numbers.
            return false;
        case Kind::BatteryHigh:
            return on_bike && f.elec.vbat > cfg_.batt_high_v;
        case Kind::BatteryLow:
            return on_bike && f.engine.rpm > cfg_.charging_rpm &&
                   f.elec.vbat < cfg_.batt_low_v;
        case Kind::FrontWheel:
            return f.health[Dev::FrontWheel].state == DevState::Fault;
        case Kind::RearWheel:
            return f.health[Dev::RearWheel].state == DevState::Fault;
        case Kind::GpsLost:
            return f.lap.phase == lap::Phase::Timing && !f.gnss.fix;
        case Kind::SdCard:
            return f.health[Dev::Sd].state == DevState::Fault;
        case Kind::ShiftLamps:
            return f.health[Dev::LedDriver].state == DevState::Fault;
        case Kind::Imu:
            return f.health[Dev::Imu].state == DevState::Fault;
        case Kind::Service:
            return f.trip.service_due;
        default:
            return false;
    }
}

Kind Alerter::update(const state::Frame& f) {
    const uint32_t now = f.t_ms;
    Kind best = Kind::None;

    for (uint8_t i = 1; i < kN; ++i) {
        const Kind k = static_cast<Kind>(i);
        const bool c = condition(k, f);

        if (c) {
            gone_[i] = false;
            if (!active_[i]) {
                if (!pending_[i]) { pending_[i] = true; since_[i] = now; }
                if (now - since_[i] >= onset_ms(k)) {
                    active_[i] = true;
                    pending_[i] = false;
                }
            }
        } else {
            pending_[i] = false;              // a flicker restarts the onset
            if (active_[i]) {
                if (!gone_[i]) { gone_[i] = true; since_[i] = now; }
                if (now - since_[i] >= cfg_.clear_ms) {
                    active_[i] = false;
                    gone_[i] = false;
                    acked_[i] = false;        // genuinely over: may fire again
                }
            }
        }

        if (active_[i] && !acked_[i] && best == Kind::None) best = k;
    }

    shown_ = best;
    return best;
}

void Alerter::acknowledge() {
    if (shown_ != Kind::None) acked_[static_cast<uint8_t>(shown_)] = true;
    shown_ = Kind::None;
}

}  // namespace core::alert
