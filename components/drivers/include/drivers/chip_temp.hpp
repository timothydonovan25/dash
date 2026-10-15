// The ESP32-P4's own die temperature sensor.
//
// The die runs warmer than the air around it, so this is not an ambient
// reading; it is how hot the dash's own brain is getting, which in a
// sun-baked number plate is the thing worth watching. DIAG shows it.
#pragma once
#include "hal/status.hpp"

namespace drivers {

class ChipTemp {
public:
    hal::Status init();
    hal::Status read(float& celsius);

private:
    void* handle_ = nullptr;     // temperature_sensor_handle_t
};

}  // namespace drivers
