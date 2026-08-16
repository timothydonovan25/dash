#include "drivers/mcp23s17.hpp"
#include "bsp/pins.hpp"

#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace drivers {
namespace {

// Register map with IOCON.BANK = 0, which is the reset default and what the
// init below keeps.
constexpr uint8_t kIodirA = 0x00, kIodirB = 0x01;
constexpr uint8_t kGppuA  = 0x0C, kGppuB  = 0x0D;
constexpr uint8_t kGpioA  = 0x12, kGpioB  = 0x13;
constexpr uint8_t kOlatA  = 0x14, kOlatB  = 0x15;
constexpr uint8_t kIocon  = 0x0A;

// Opcode byte: 0100 A2 A1 A0 R/W. The board strap is address 0.
constexpr uint8_t kOpWrite = 0x40;
constexpr uint8_t kOpRead  = 0x41;

hal::Status from_esp(int err) {
    if (err == ESP_OK) return hal::Status::Ok;
    if (err == ESP_ERR_TIMEOUT) return hal::Status::Timeout;
    if (err == ESP_ERR_INVALID_STATE) return hal::Status::Busy;
    return hal::Status::IoError;
}

}  // namespace

hal::Status Mcp23s17::write_reg(uint8_t reg, uint8_t value) {
    if (!dev_) return hal::Status::NotPresent;
    uint8_t tx[3] = {kOpWrite, reg, value};
    spi_transaction_t t = {};
    t.length = 8 * sizeof tx;
    t.tx_buffer = tx;
    return from_esp(spi_device_polling_transmit(
        static_cast<spi_device_handle_t>(dev_), &t));
}

hal::Status Mcp23s17::read_reg(uint8_t reg, uint8_t& value) {
    if (!dev_) return hal::Status::NotPresent;
    uint8_t tx[3] = {kOpRead, reg, 0x00};
    uint8_t rx[3] = {};
    spi_transaction_t t = {};
    t.length = 8 * sizeof tx;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    const hal::Status s = from_esp(spi_device_polling_transmit(
        static_cast<spi_device_handle_t>(dev_), &t));
    if (hal::ok(s)) value = rx[2];
    return s;
}

hal::Status Mcp23s17::init() {
    if (!dev_) {
        spi_device_interface_config_t cfg = {};
        // 10 MHz is the datasheet ceiling for this part at 3V3, and it is
        // the reason each device on SPI2 carries its own config.
        cfg.clock_speed_hz = bsp::kSpiHzMcp;
        cfg.mode = 0;
        cfg.spics_io_num = bsp::kCsMcp;
        cfg.queue_size = 4;
        spi_device_handle_t h = nullptr;
        const esp_err_t err = spi_bus_add_device(SPI2_HOST, &cfg, &h);
        if (err != ESP_OK) return from_esp(err);
        dev_ = h;
    }

    // GPA0..7 are the shift lamps, outputs. GPB5..7 are neutral, TFT_RST and
    // card-detect: RST is an output, the other two are inputs with pull-ups.
    dir_[0] = 0x00;                 // all of A out
    dir_[1] = 0xA0;                 // GPB5 and GPB7 in, GPB6 (RST) out
    pullup_[1] = 0xA0;
    return refresh_config();
}

hal::Status Mcp23s17::refresh_config() {
    // Re-writing every configuration register, not just re-initialising.
    //
    // A browned-out expander comes back with default registers: it still
    // answers, it still returns data, and every lamp pin is an input again.
    // It looks completely alive and drives nothing. The keeper calls this
    // periodically for exactly that reason (§12).
    hal::Status s = write_reg(kIocon, 0x28);     // SEQOP off, HAEN on
    if (!hal::ok(s)) return s;
    s = write_reg(kIodirA, dir_[0]);
    if (!hal::ok(s)) return s;
    s = write_reg(kIodirB, dir_[1]);
    if (!hal::ok(s)) return s;
    s = write_reg(kGppuA, pullup_[0]);
    if (!hal::ok(s)) return s;
    s = write_reg(kGppuB, pullup_[1]);
    if (!hal::ok(s)) return s;
    s = write_reg(kOlatA, out_[0]);
    if (!hal::ok(s)) return s;
    return write_reg(kOlatB, out_[1] | (1u << (bsp::mcp::kTftRst - 8)));
}

hal::Status Mcp23s17::read_port(uint8_t port, uint8_t& value) {
    return read_reg(port == 0 ? kGpioA : kGpioB, value);
}

hal::Status Mcp23s17::write_port(uint8_t port, uint8_t value) {
    out_[port & 1] = value;
    return write_reg(port == 0 ? kOlatA : kOlatB, value);
}

hal::Status Mcp23s17::set_direction(uint8_t port, uint8_t input_mask) {
    dir_[port & 1] = input_mask;
    return write_reg(port == 0 ? kIodirA : kIodirB, input_mask);
}

hal::Status Mcp23s17::set_pullups(uint8_t port, uint8_t mask) {
    pullup_[port & 1] = mask;
    return write_reg(port == 0 ? kGppuA : kGppuB, mask);
}

hal::Status Mcp23s17::write_shift_lamps(uint8_t mask) {
    // Straight through: core::shift::pattern() returns bit 0 = lamp 1, and
    // the lamps are wired GPA0..GPA7 in that order. One SPI write, about
    // 20 us, and it is the last link in the 3 ms tach-to-lamp budget.
    return write_port(0, mask);
}

hal::Status Mcp23s17::pulse_tft_reset() {
    const uint8_t bit = 1u << (bsp::mcp::kTftRst - 8);
    uint8_t b = out_[1];

    hal::Status s = write_port(1, static_cast<uint8_t>(b & ~bit));
    if (!hal::ok(s)) return s;
    // ILI9488 wants at least 10 us low; 10 ms is free at boot and removes
    // any argument about it.
    vTaskDelay(pdMS_TO_TICKS(10));
    s = write_port(1, static_cast<uint8_t>(b | bit));
    vTaskDelay(pdMS_TO_TICKS(120));      // datasheet: 120 ms before commands
    return s;
}

hal::Status Mcp23s17::poll_inputs() {
    uint8_t b = 0;
    const hal::Status s = read_port(1, b);
    if (!hal::ok(s)) return s;

    const uint8_t neutral_bit = 1u << (bsp::mcp::kNeutral - 8);
    const bool raw = (b & neutral_bit) != 0;
    // Opto-isolated and pulled up: LOW means in neutral. Inverted here, once,
    // so no screen and no service ever has to know.
    neutral_ = bsp::mcp::kNeutralActiveLow ? !raw : raw;

    const uint8_t cd_bit = 1u << (bsp::mcp::kSdCardDetect - 8);
    card_detect_ = (b & cd_bit) == 0;
    return hal::Status::Ok;
}

}  // namespace drivers
