#include "core/flash/flash.hpp"

namespace core::flash {

bool still(const Stillness& s) {
    // Written as "not below" so a NaN from any source reads as moving.
    if (!(s.front_mph < kStillWheelMph)) return false;
    if (!(s.rear_mph < kStillWheelMph)) return false;
    if (s.gps_fix && !(s.gps_mph < kStillGpsMph)) return false;
    if (!(s.rpm < 1.0f)) {
        // Turning, or NaN. Half the first lamp's rpm: idle is well under it,
        // and a blip that reaches it during a write has a few hundred ms of
        // margin to get there.
        if (!(s.rpm * 2.0f < static_cast<float>(s.lamp_base_rpm))) return false;
    }
    return true;
}

}  // namespace core::flash
