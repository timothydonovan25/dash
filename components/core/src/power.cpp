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

}  // namespace core::power
