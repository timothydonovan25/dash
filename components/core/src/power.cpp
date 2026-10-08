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

}  // namespace core::power
