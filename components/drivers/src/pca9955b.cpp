#include "drivers/pca9955b.hpp"
#include "bsp/pins.hpp"
#include "core/shift/shift.hpp"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdio>
#include "esp_rom_sys.h"

namespace drivers {

// The mask from core/shift goes straight onto OUT0..OUT7.
static_assert(core::shift::kLampCount == bsp::kLedLampCount,
              "core/shift and the LED driver disagree about lamp count");
static_assert(bsp::kLedLamp0 == 0,
              "the lamp mask maps bit n to OUTn; lamp 1 must be OUT0");
static_assert(bsp::kLedBacklight0 + bsp::kLedBacklightCount <= 16 &&
              bsp::kLedBacklight0 >= bsp::kLedLamp0 + bsp::kLedLampCount,
              "backlight channels must sit above the lamps, inside OUT0..15");

namespace {

// A transfer here is ~0.2 ms at 1 MHz, and `ctrl` makes it inside a 2 ms
// tick: a wedged bus must cost a tick, not five of them.
constexpr int kTimeoutMs = 5;   // DIAGNOSTIC: 100 kHz needs ~1.7 ms for the largest write

// Register map, datasheet Rev. 2.3 Table 7.
constexpr uint8_t kMode1   = 0x00;
constexpr uint8_t kMode2   = 0x01;
constexpr uint8_t kPwm0    = 0x08;
constexpr uint8_t kIref0   = 0x18;
constexpr uint8_t kEflag0  = 0x46;
// MODE2 bit 4, write-only and self-clearing: clears EFLAGn and ERROR.
constexpr uint8_t kClrErr  = 0x10;
// Bit 7 of the register byte sets the auto-increment flag. With MODE1's
// AI1:AI0 left at 00 the pointer runs 00h..43h, so one transfer covers any
// contiguous run of registers.
constexpr uint8_t kAutoInc = 0x80;

// LEDOUTn: two bits per output. 10b is "individual PWM", which is the only
// mode used; 00b is off for the outputs nothing is wired to.
constexpr uint8_t kLdrPwm  = 0b10;

// 100 % is 254, not 255: the chip skips its open/short test on an output at
// PWM 255 (datasheet §7.3.15), so full brightness would never be checked.
// One step in 255 is not visible.
uint8_t duty_of(uint8_t percent) {
    if (percent > 100) percent = 100;
    return static_cast<uint8_t>((254u * percent) / 100u);
}

// DIAGNOSTIC: outcome counts for every transfer since boot. File statics on
// purpose: this build exists to find one board fault and is not for main.
std::atomic<uint32_t> g_ok{0}, g_nack{0}, g_timeout{0}, g_state{0}, g_other{0};
std::atomic<int32_t>  g_last_err{0};
std::atomic<uint32_t> g_last_err_ms{0};
std::atomic<uint8_t>  g_last_reg{0};

void count(esp_err_t err, uint8_t reg) {
    if (err == ESP_OK) { g_ok.fetch_add(1, std::memory_order_relaxed); return; }
    if (err == ESP_ERR_INVALID_RESPONSE) g_nack.fetch_add(1, std::memory_order_relaxed);
    else if (err == ESP_ERR_TIMEOUT) g_timeout.fetch_add(1, std::memory_order_relaxed);
    else if (err == ESP_ERR_INVALID_STATE) g_state.fetch_add(1, std::memory_order_relaxed);
    else g_other.fetch_add(1, std::memory_order_relaxed);
    g_last_err.store(err, std::memory_order_relaxed);
    g_last_reg.store(reg, std::memory_order_relaxed);
    g_last_err_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000), std::memory_order_relaxed);
}

}  // namespace

void Pca9955b::diag_report(char* buf, size_t n) {
    int k = std::snprintf(buf, n,
        "led: %d Hz, ok %u, nack %u, timeout %u, bus-state %u, other %u; last %s (0x%x) at reg 0x%02x, %u ms ago\n",
        bsp::kLedI2cHz, (unsigned)g_ok.load(), (unsigned)g_nack.load(), (unsigned)g_timeout.load(),
        (unsigned)g_state.load(), (unsigned)g_other.load(),
        g_last_err.load() ? esp_err_to_name(g_last_err.load()) : "none", (unsigned)g_last_err.load(),
        g_last_reg.load(),
        g_last_err.load() ? (unsigned)(esp_timer_get_time() / 1000 - g_last_err_ms.load()) : 0u);
    if (!bus_ || k < 0 || static_cast<size_t>(k) >= n) return;
    auto bus = static_cast<i2c_master_bus_handle_t>(bus_);
    // Idle levels, straight from the GPIO input registers: between `ctrl`'s
    // ~3 transfers a second an idle I2C bus is high. SDA and SCL are open
    // drain with input on; BOOT (GPIO35) is a plain input. TFT_D0 (GPIO33) is
    // the LCD peripheral's output, and reads only if its input buffer is on.
    auto lvl = [](int g) -> int {
        const uint32_t v = g < 32 ? REG_READ(GPIO_IN_REG) : REG_READ(GPIO_IN1_REG);
        return (v >> (g % 32)) & 1;
    };
    int low_sda = 0, low_scl = 0, low_boot = 0, d0_hi = 0, both_low = 0;
    constexpr int kSamples = 4000;
    for (int i = 0; i < kSamples; ++i) {
        const int sda = lvl(bsp::kLedSda), scl = lvl(bsp::kLedScl);
        low_sda += !sda; low_scl += !scl; low_boot += !lvl(35); d0_hi += lvl(33);
        both_low += (!sda && !scl);
        esp_rom_delay_us(25);
    }
    k += std::snprintf(buf + k, n - k,
        "led: idle samples %d over 100 ms: SDA low %d, SCL low %d, both low %d, BOOT(35) low %d, TFT_D0(33) reads high %d\n",
        kSamples, low_sda, low_scl, both_low, low_boot, d0_hi);
    // Who answers on I2C1: the chip at 0x15, and anything else (a second
    // address would mean AD pins misread, or a short to another net).
    k += std::snprintf(buf + k, n - k, "led: probe 0x%02x: %s; answering:", bsp::kLedAddr,
                       esp_err_to_name(i2c_master_probe(bus, bsp::kLedAddr, 20)));
    for (uint16_t a = 0x08; a < 0x78 && static_cast<size_t>(k) < n - 8; ++a) {
        if (i2c_master_probe(bus, a, 5) == ESP_OK) k += std::snprintf(buf + k, n - k, " 0x%02x", a);
    }
    k += std::snprintf(buf + k, n - k, "\n");
    // The chip's own view: MODE1 (0x00 when configured; 0x89 is its
    // power-on value, so a reset since the last refresh shows here),
    // MODE2 (bit 7 OVERTEMP, bit 6 ERROR), EFLAG0..3 (open/short per output),
    // and IREF0 / IREF8 (0 after a brown-out).
    uint8_t m[2] = {}, ef[4] = {}, iref0 = 0, iref8 = 0;
    const hal::Status a = read(kMode1, m, 2);
    const hal::Status b = read(kEflag0, ef, 4);
    const hal::Status c = read(kIref0, &iref0, 1);
    const hal::Status d = read(static_cast<uint8_t>(kIref0 + 8), &iref8, 1);
    std::snprintf(buf + k, n - k,
        "led: MODE1 %02x MODE2 %02x%s%s EFLAG %02x %02x %02x %02x IREF0 %02x IREF8 %02x (reads %s %s %s %s)\n",
        m[0], m[1], (m[1] & 0x80) ? " OVERTEMP" : "", (m[1] & 0x40) ? " ERROR" : "",
        ef[0], ef[1], ef[2], ef[3], iref0, iref8,
        hal::status_name(a), hal::status_name(b), hal::status_name(c), hal::status_name(d));
}

void Pca9955b::diag_drive(char* buf, size_t n) {
    auto lvl = [](int g) -> int {
        const uint32_t v = g < 32 ? REG_READ(GPIO_IN_REG) : REG_READ(GPIO_IN1_REG);
        return (v >> (g % 32)) & 1;
    };
    const int sda0 = lvl(bsp::kLedSda), boot0 = lvl(35);
    // BOOT as a push-pull output, high, briefly; against a hard short the
    // pad's drive limit is the only current, and only for microseconds.
    gpio_set_level(GPIO_NUM_35, 1);
    gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_delay_us(10);
    const int boot_hi = lvl(35), sda_hi = lvl(bsp::kLedSda);
    gpio_set_level(GPIO_NUM_35, 0);
    esp_rom_delay_us(10);
    const int boot_lo = lvl(35), sda_lo = lvl(bsp::kLedSda);
    gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_35, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(50);
    const int boot_after = lvl(35);
    int k = std::snprintf(buf, n,
        "led: drive BOOT: before SDA %d BOOT %d | BOOT driven 1 -> BOOT %d SDA %d | driven 0 -> BOOT %d SDA %d | released BOOT %d\n",
        sda0, boot0, boot_hi, sda_hi, boot_lo, sda_lo, boot_after);
    if (!bus_ || k < 0 || static_cast<size_t>(k) >= n) return;
    const esp_err_t r = i2c_master_bus_reset(static_cast<i2c_master_bus_handle_t>(bus_));
    int low = 0;
    for (int i = 0; i < 1000; ++i) { low += !lvl(bsp::kLedSda); esp_rom_delay_us(20); }
    std::snprintf(buf + k, n - k, "led: bus reset %s; SDA low %d of 1000 after\n", esp_err_to_name(r), low);
}

hal::Status Pca9955b::write(uint8_t reg, const uint8_t* data, size_t n) {
    if (!dev_) return hal::Status::NotPresent;
    // The largest run written is sixteen registers plus the address byte.
    uint8_t buf[1 + 16];
    if (n > 16) return hal::Status::Unsupported;
    buf[0] = static_cast<uint8_t>(kAutoInc | reg);
    for (size_t i = 0; i < n; ++i) buf[1 + i] = data[i];
    const esp_err_t err = i2c_master_transmit(
        static_cast<i2c_master_dev_handle_t>(dev_), buf, n + 1, kTimeoutMs);
    count(err, reg);
    if (err == ESP_OK) return hal::Status::Ok;
    // ESP_ERR_INVALID_RESPONSE is a NACK or the hardware's own SCL timeout;
    // either way the part did not take the write.
    return err == ESP_ERR_TIMEOUT ? hal::Status::Timeout : hal::Status::IoError;
}

hal::Status Pca9955b::read(uint8_t reg, uint8_t* data, size_t n) {
    if (!dev_) return hal::Status::NotPresent;
    const uint8_t addr = static_cast<uint8_t>(kAutoInc | reg);
    const esp_err_t err = i2c_master_transmit_receive(
        static_cast<i2c_master_dev_handle_t>(dev_), &addr, 1, data, n, kTimeoutMs);
    count(err, static_cast<uint8_t>(0x80 | reg));   // bit 7: a read
    if (err == ESP_OK) return hal::Status::Ok;
    return err == ESP_ERR_TIMEOUT ? hal::Status::Timeout : hal::Status::IoError;
}

hal::Status Pca9955b::read_faults(uint8_t& mode2, uint8_t eflag[4]) {
    hal::Status s = read(kMode2, &mode2, 1);
    if (!hal::ok(s)) return s;
    return read(kEflag0, eflag, 4);
}

hal::Status Pca9955b::refresh_config() {
    // MODE1 = 0: normal mode, auto-increment over the whole map, and no
    // answer to the sub-call or all-call addresses — this is the only
    // device on its bus, and a group address nobody uses is just another
    // way to write it by accident. MODE2: outputs change on STOP, so the
    // eight lamp duties in one transfer land together, and CLRERR, so the
    // error flags start again from here (read_faults).
    uint8_t head[6] = {0x00, kClrErr, 0, 0, 0, 0};
    for (int ch = 0; ch < 16; ++ch) {
        const bool lamp = ch >= bsp::kLedLamp0 &&
                          ch < bsp::kLedLamp0 + bsp::kLedLampCount;
        const bool back = ch >= bsp::kLedBacklight0 &&
                          ch < bsp::kLedBacklight0 + bsp::kLedBacklightCount;
        if (lamp || back) head[2 + ch / 4] |= static_cast<uint8_t>(kLdrPwm << (2 * (ch % 4)));
    }
    hal::Status s = write(kMode1, head, sizeof head);
    if (!hal::ok(s)) return s;

    uint8_t iref[16] = {};
    for (int i = 0; i < bsp::kLedLampCount; ++i) {
        iref[bsp::kLedLamp0 + i] = bsp::kLedLampIrefMax;
    }
    for (int i = 0; i < bsp::kLedBacklightCount; ++i) {
        iref[bsp::kLedBacklight0 + i] = bsp::kLedBacklightIrefMax;
    }
    return write(kIref0, iref, sizeof iref);
}

hal::Status Pca9955b::init() {
    if (!dev_) {
        if (!bus_) return hal::Status::NotPresent;
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = bsp::kLedAddr;
        cfg.scl_speed_hz = bsp::kLedI2cHz;
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(bus_),
                                      &cfg, &dev) != ESP_OK) {
            return hal::Status::IoError;
        }
        dev_ = dev;
    }
    // Dark first, then configured: if the configuration write fails half
    // way, nothing is left lit at a duty nobody chose.
    const uint8_t zero[16] = {};
    hal::Status s = write(kPwm0, zero, sizeof zero);
    if (!hal::ok(s)) return s;
    mask_ = 0;
    lamp_pwm_ = 0;
    return refresh_config();
}

hal::Status Pca9955b::set_mask(uint8_t mask) {
    // One transfer, OUT0..OUT7: 10 bytes, about 0.1 ms at 1 MHz. This is the
    // last link in the 3 ms tach-to-lamp budget.
    mask_ = mask;
    uint8_t pwm[bsp::kLedLampCount];
    for (int i = 0; i < bsp::kLedLampCount; ++i) {
        pwm[i] = (mask >> i) & 1u ? lamp_pwm_ : 0;
    }
    return write(static_cast<uint8_t>(kPwm0 + bsp::kLedLamp0), pwm, sizeof pwm);
}

hal::Status Pca9955b::set_master(uint8_t percent) {
    lamp_pwm_ = duty_of(percent);
    return set_mask(mask_);
}

hal::Status Pca9955b::blank_lamps() {
    const uint8_t zero[bsp::kLedLampCount] = {};
    return write(static_cast<uint8_t>(kPwm0 + bsp::kLedLamp0), zero, sizeof zero);
}

hal::Status Pca9955b::set_backlight(uint8_t percent) {
    const uint8_t d = duty_of(percent);
    uint8_t pwm[bsp::kLedBacklightCount];
    for (int i = 0; i < bsp::kLedBacklightCount; ++i) pwm[i] = d;
    return write(static_cast<uint8_t>(kPwm0 + bsp::kLedBacklight0), pwm, sizeof pwm);
}

}  // namespace drivers
