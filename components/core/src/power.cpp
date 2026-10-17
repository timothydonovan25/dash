#include "core/power/power.hpp"

namespace core::power {

float vin_from_pin_mv(uint32_t pin_mv, const Divider& d) {
    if (d.bottom_kohm <= 0.0f) return 0.0f;
    const float ratio = (d.top_kohm + d.bottom_kohm) / d.bottom_kohm;
    return static_cast<float>(pin_mv) * ratio / 1000.0f;
}

Source classify(float vin_v, const Thresholds& t) {
    if (!(vin_v >= t.present_v)) return Source::Unknown;   // also catches NaN
    return vin_v >= t.bike_v ? Source::Bike : Source::Usb;
}

uint8_t lamp_master_pct(bool on_bike, uint8_t setting_pct, uint8_t usb_cap_pct) {
    const uint8_t want = setting_pct > 100 ? 100 : setting_pct;
    if (on_bike) return want;
    return want < usb_cap_pct ? want : usb_cap_pct;
}

FailDetector::Event FailDetector::feed(float v) {
    switch (state_) {
        case State::Idle:
            run_ = v >= cfg_.arm_v ? static_cast<uint16_t>(run_ + 1) : 0;
            if (run_ >= cfg_.arm_n) { state_ = State::Armed; run_ = 0; }
            return Event::None;

        case State::Armed:
            run_ = v < cfg_.trip_v ? static_cast<uint16_t>(run_ + 1) : 0;
            if (run_ >= cfg_.trip_n) {
                state_ = State::Tripped;
                low_run_ = run_;
                run_ = 0;
                since_trip_ = 0;
                return Event::Tripped;
            }
            return Event::None;

        case State::Tripped:
            if (!(v < cfg_.trip_v)) low_run_ = 0;
            else if (low_run_ < 0xFFFF) ++low_run_;
            run_ = v >= cfg_.ok_v ? static_cast<uint16_t>(run_ + 1) : 0;
            if (run_ >= cfg_.ok_n) {
                state_ = State::Armed;
                run_ = 0;
                return Event::Recovered;
            }
            if (since_trip_ < 0xFFFF) ++since_trip_;
            if (since_trip_ >= cfg_.usb_n) {
                state_ = State::Idle;      // re-arms only on a bike supply
                run_ = 0;
                return Event::Recovered;
            }
            return Event::None;
    }
    return Event::None;
}

}  // namespace core::power
