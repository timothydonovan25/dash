#include "drivers/vin_sense.hpp"
#include "bsp/pins.hpp"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

namespace drivers {
namespace {

// 12 dB covers the pin's whole range: 2.6 V on a running bike, and the
// 3.3 V clamp above that.
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;

// Averaged, because ignition noise rides on VIN and the reading feeds a
// 1 Hz slot, not a control loop.
constexpr int kSamples = 8;

}  // namespace

hal::Status VinSense::init() {
    if (unit_) return hal::Status::Ok;

    adc_unit_t unit;
    adc_channel_t chan;
    // The channel comes from the pin, so bsp/pins.hpp stays the only place a
    // pin number is written down.
    if (adc_oneshot_io_to_channel(bsp::kVinSense, &unit, &chan) != ESP_OK) {
        return hal::Status::Unsupported;
    }

    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = unit;
    adc_oneshot_unit_handle_t handle = nullptr;
    if (adc_oneshot_new_unit(&ucfg, &handle) != ESP_OK) return hal::Status::IoError;

    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = kAtten;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(handle, chan, &ccfg) != ESP_OK) {
        adc_oneshot_del_unit(handle);
        return hal::Status::IoError;
    }
    unit_ = handle;
    channel_ = static_cast<int>(chan);

    // Without calibration the reading is only roughly millivolts. That is
    // still enough to tell 0.8 V from 2.6 V, which is the decision that
    // matters, so a failed calibration is not a failed init.
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = unit;
    cal.chan = chan;
    cal.atten = kAtten;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_cali_handle_t cali = nullptr;
    if (adc_cali_create_scheme_curve_fitting(&cal, &cali) == ESP_OK) cali_ = cali;
#endif
    return hal::Status::Ok;
}

hal::Status VinSense::read_vin_pin_mv(uint32_t& mv) {
    if (!unit_) return hal::Status::NotPresent;
    auto* unit = static_cast<adc_oneshot_unit_handle_t>(unit_);
    const auto chan = static_cast<adc_channel_t>(channel_);

    int sum = 0;
    for (int i = 0; i < kSamples; ++i) {
        int raw = 0;
        if (adc_oneshot_read(unit, chan, &raw) != ESP_OK) return hal::Status::IoError;
        sum += raw;
    }
    const int raw = sum / kSamples;

    int out_mv = 0;
    if (cali_ && adc_cali_raw_to_voltage(static_cast<adc_cali_handle_t>(cali_),
                                         raw, &out_mv) == ESP_OK) {
        mv = static_cast<uint32_t>(out_mv < 0 ? 0 : out_mv);
        return hal::Status::Ok;
    }
    // Uncalibrated: 12-bit full scale is roughly 3.1 V at 12 dB.
    mv = static_cast<uint32_t>(raw) * 3100u / 4095u;
    return hal::Status::Ok;
}

}  // namespace drivers
