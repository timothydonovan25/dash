// See pod_buttons.hpp.
#include "pod_buttons.hpp"
#include "p4_pins.hpp"
#include "core/input/input.hpp"

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace demo {
namespace {

constexpr const char* kTag = "pod_buttons";

constexpr uint8_t kIodirA = 0x00, kIpolA = 0x02, kGppuA = 0x0C, kGpioA = 0x12;

// The bus is shared with touch, so a missing pod must cost a NACK, not a
// stall, the same reasoning as drivers/src/pod.cpp.
constexpr int kTimeoutMs = 10;

// How often to check a working pod still holds its configuration, and how
// often to look for one that is not answering. The second is short so DIAG
// shows a pulled cable within a second: a NACK costs ~0.1 ms of the bus.
constexpr uint32_t kCheckMs = 1000;
constexpr uint32_t kRetryMs = 100;

auto dev_of(void* p) { return static_cast<i2c_master_dev_handle_t>(p); }

hal::Status from_esp(esp_err_t err) {
    switch (err) {
        case ESP_OK: return hal::Status::Ok;
        case ESP_ERR_TIMEOUT: return hal::Status::Timeout;
        case ESP_ERR_NOT_FOUND:
        case ESP_FAIL: return hal::Status::NotPresent;   // NACK
        default: return hal::Status::IoError;
    }
}

hal::Status write_reg(void* dev, uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return from_esp(i2c_master_transmit(dev_of(dev), buf, sizeof buf, kTimeoutMs));
}

hal::Status read_reg(void* dev, uint8_t reg, uint8_t& value) {
    return from_esp(i2c_master_transmit_receive(dev_of(dev), &reg, 1, &value, 1,
                                                kTimeoutMs));
}

}  // namespace

hal::Status PodButtons::init(void* bus) {
    if (!bus) return hal::Status::NotPresent;

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = p4pins::kPodAddr;
    // 100 kHz, as on the bike: the pod sits on the end of its own cable.
    cfg.scl_speed_hz = 100'000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(bus),
                                  &cfg, &dev) != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_add_device failed");
        return hal::Status::IoError;
    }
    dev_ = dev;

    const hal::Status s = configure();
    ESP_LOGI(kTag, "pod on CN3 (0x%02x): %s", p4pins::kPodAddr, hal::status_name(s));
    return s;
}

hal::Status PodButtons::configure() {
    // Exactly drivers::ButtonPod::refresh_config(): every port-A pin an
    // input with its pull-up, inverted in the chip so pressed reads as 1.
    configured_ = false;
    hal::Status s = write_reg(dev_, kIodirA, 0xFF);
    if (hal::ok(s)) s = write_reg(dev_, kGppuA, 0xFF);
    if (hal::ok(s)) s = write_reg(dev_, kIpolA, 0xFF);
    configured_ = hal::ok(s);
    return s;
}

hal::Status PodButtons::read_mask(uint8_t& mask_out, uint32_t now_ms) {
    mask_out = 0;
    last_gpa_ = 0;
    if (!dev_) return hal::Status::NotPresent;

    if (static_cast<int32_t>(now_ms - next_config_ms_) >= 0) {
        // A pod that was unplugged and plugged back in answers fine but is
        // at its power-on defaults: no pull-ups, no inversion, buttons
        // floating. IPOL is the cheapest register to tell that by.
        uint8_t ipol = 0;
        const bool was = configured_;
        hal::Status s = hal::Status::Ok;
        if (!configured_ || !hal::ok(read_reg(dev_, kIpolA, ipol)) || ipol != 0xFF) {
            s = configure();
            if (configured_ != was)
                ESP_LOGI(kTag, "pod %s", configured_ ? "connected" : "gone");
        }
        next_config_ms_ = now_ms + (configured_ ? kCheckMs : kRetryMs);
        if (!hal::ok(s)) return s;
    } else if (!configured_) {
        return hal::Status::Busy;   // between retries: no I/O, nothing new
    }

    uint8_t gpa = 0;
    const hal::Status s = read_reg(dev_, kGpioA, gpa);
    last_gpa_ = hal::ok(s) ? gpa : 0;
    if (!hal::ok(s)) {
        configured_ = false;
        next_config_ms_ = now_ms + kRetryMs;
        ESP_LOGW(kTag, "pod gone: %s", hal::status_name(s));
        return s;
    }
    mask_out = core::input::pod_pins_to_buttons(gpa);
    return hal::Status::Ok;
}

}  // namespace demo
