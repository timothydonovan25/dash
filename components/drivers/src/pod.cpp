#include "drivers/pod.hpp"
#include "bsp/pins.hpp"
#include "core/input/input.hpp"
#include "bsp/buses.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"

namespace drivers {
namespace {

constexpr uint8_t kIodirA = 0x00, kGppuA = 0x0C, kGpioA = 0x12, kIpolA = 0x02;

// Short, on purpose. A pod that has fallen off its connector must not stall
// the 100 Hz poll loop; it must fail fast and be counted.
constexpr int kTimeoutMs = 10;

// Half a bit at 100 kHz. Recovery clocks the bus at the speed the bus runs
// at, not as fast as the CPU can toggle a pin: the pod is half a metre away
// behind 4.7k pull-ups and the rise time is about 470 ns.
constexpr uint32_t kHalfPeriodUs = 5;

hal::Status from_esp(int err) {
    switch (err) {
        case ESP_OK: return hal::Status::Ok;
        case ESP_ERR_TIMEOUT: return hal::Status::Timeout;
        // A NACK (nobody home), or the hardware's SCL timeout; the
        // i2c_master driver does not tell the two apart.
        case ESP_ERR_INVALID_RESPONSE: return hal::Status::NotPresent;
        case ESP_ERR_INVALID_STATE: return hal::Status::Busy;
        default: return hal::Status::IoError;
    }
}

}  // namespace

hal::Status ButtonPod::write_reg(uint8_t reg, uint8_t value) {
    if (!dev_) return hal::Status::NotPresent;
    const uint8_t buf[2] = {reg, value};
    return from_esp(i2c_master_transmit(static_cast<i2c_master_dev_handle_t>(dev_),
                                        buf, sizeof buf, kTimeoutMs));
}

hal::Status ButtonPod::read_reg(uint8_t reg, uint8_t& value) {
    if (!dev_) return hal::Status::NotPresent;
    return from_esp(i2c_master_transmit_receive(
        static_cast<i2c_master_dev_handle_t>(dev_), &reg, 1, &value, 1, kTimeoutMs));
}

hal::Status ButtonPod::init() {
    // The bus itself belongs to bsp (pull-ups, pins). This driver only ever
    // adds a device to it, at the pod's 100 kHz.
    if (!dev_) {
        if (!bus_) return hal::Status::NotPresent;
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = bsp::kPodAddr;
        cfg.scl_speed_hz = bsp::kI2cHz;
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(bus_),
                                      &cfg, &dev) != ESP_OK) {
            return hal::Status::IoError;
        }
        dev_ = dev;
    }
    return refresh_config();
}

hal::Status ButtonPod::refresh_config() {
    // Five buttons on GPA3..GPA7 (core::input::kPodPin), wired to ground,
    // with pull-ups. GPA0..2 are not connected. IPOL
    // inverts them in the chip, so a pressed button reads as a 1 here and
    // the polarity question is answered once, in hardware config, rather
    // than in every caller.
    hal::Status s = write_reg(kIodirA, 0xFF);
    if (!hal::ok(s)) return s;
    s = write_reg(kGppuA, 0xFF);
    if (!hal::ok(s)) return s;
    return write_reg(kIpolA, 0xFF);
}

hal::Status ButtonPod::read_buttons(uint8_t& pressed_mask) {
    uint8_t v = 0;
    const hal::Status s = read_reg(kGpioA, v);
    if (!hal::ok(s)) return s;
    last_gpa_ = v;
    pressed_mask = core::input::pod_pins_to_buttons(v);   // bit 0 = A
    return hal::Status::Ok;
}

hal::Status ButtonPod::recover_bus() {
    // A slave interrupted mid-byte can be left holding SDA low, waiting to
    // finish a transfer nobody is clocking any more. Here that is the pod
    // browning out over a jump. The master cannot issue a START while SDA is
    // low, so from then on *every* transaction fails and no button on the
    // bike works until the power is cycled.
    //
    // Tearing the driver down and putting it back does not fix it: the
    // peripheral does not drive SCL while idle, so the slave never gets the
    // edges it is waiting for. The bus has to be clocked by hand.
    if (dev_) {
        i2c_master_bus_rm_device(static_cast<i2c_master_dev_handle_t>(dev_));
        dev_ = nullptr;
    }
    bsp::release_pod_bus();
    bus_ = nullptr;

    const gpio_num_t sda = static_cast<gpio_num_t>(bsp::kI2cSda);
    const gpio_num_t scl = static_cast<gpio_num_t>(bsp::kI2cScl);

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << bsp::kI2cSda) | (1ULL << bsp::kI2cScl);
    // Open drain, and readable: I2C lines are only ever pulled down, never
    // driven high, and we have to watch SDA to know when to stop.
    io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) return hal::Status::IoError;
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);

    // Nine, because a byte plus its ACK is the longest a slave can still be
    // holding out for. Stop as soon as SDA comes back: every extra edge is
    // another chance to be read as the start of something new.
    last_clocks_ = 0;
    for (int i = 0; i < 9 && gpio_get_level(sda) == 0; ++i) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(kHalfPeriodUs);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(kHalfPeriodUs);
        ++last_clocks_;
    }
    const bool freed = gpio_get_level(sda) != 0;

    // A STOP — SDA rising while SCL is high — so the slave treats the next
    // START as a new transfer rather than a continuation of the old one.
    gpio_set_level(sda, 0);
    esp_rom_delay_us(kHalfPeriodUs);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(kHalfPeriodUs);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(kHalfPeriodUs);

    if (recoveries_ != UINT16_MAX) ++recoveries_;
    if (!freed && stuck_ != UINT16_MAX) ++stuck_;

    // Hand the pins back to the peripheral: a new bus, and the device on it.
    bus_ = bsp::init_pod_bus();
    if (!bus_) return hal::Status::IoError;
    const hal::Status s = init();

    // A bus still held low is a failure even when the re-init went fine.
    // Reporting Ok here would have the keeper stop retrying and the dash
    // sit there with dead buttons and a healthy-looking DIAG screen.
    return freed ? s : hal::Status::IoError;
}

}  // namespace drivers
