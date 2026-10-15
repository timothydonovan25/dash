#include "drivers/chip_temp.hpp"

#include "driver/temperature_sensor.h"

namespace drivers {

hal::Status ChipTemp::init() {
    if (handle_) return hal::Status::Ok;
    // The range sets the sensor's internal offset; -10..80 C covers a dash
    // from a cold morning to a hot afternoon at the best accuracy.
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_handle_t h = nullptr;
    if (temperature_sensor_install(&cfg, &h) != ESP_OK) return hal::Status::IoError;
    if (temperature_sensor_enable(h) != ESP_OK) {
        temperature_sensor_uninstall(h);
        return hal::Status::IoError;
    }
    handle_ = h;
    return hal::Status::Ok;
}

hal::Status ChipTemp::read(float& celsius) {
    if (!handle_) return hal::Status::NotPresent;
    return temperature_sensor_get_celsius(static_cast<temperature_sensor_handle_t>(handle_),
                                          &celsius) == ESP_OK
               ? hal::Status::Ok
               : hal::Status::IoError;
}

}  // namespace drivers
