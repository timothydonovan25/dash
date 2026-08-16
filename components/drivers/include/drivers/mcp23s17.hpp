// MCP23S17 on the shared SPI2 bus.
//
// This part is on the critical path in two ways that are easy to miss:
//
//  - **TFT_RST hangs off GPB6**, not off a GPIO. The expander therefore has
//    to come up before the panel (§6). R3 pulls that line high, so a dead
//    expander degrades to "screen may not reset cleanly" rather than to a
//    brick — but the boot order still matters.
//  - **It caps at 10 MHz**, well under the IMU's 24. That is why it gets its
//    own `spi_device_interface_config_t` rather than sharing one: the IDF
//    reconfigures the clock per device, and one bus-wide speed would either
//    cripple the IMU or overclock this.
//
// Never call spi_device_acquire_bus() here. Three tasks share SPI2 and the
// driver serialises per transaction on its own; acquiring the bus defeats
// that and turns a 20 us write into a priority inversion.
#pragma once
#include "hal/i_gpio_expander.hpp"

namespace drivers {

class Mcp23s17 final : public hal::IExpander {
public:
    hal::Status init() override;
    hal::Status read_port(uint8_t port, uint8_t& value) override;
    hal::Status write_port(uint8_t port, uint8_t value) override;
    hal::Status set_direction(uint8_t port, uint8_t input_mask) override;
    hal::Status set_pullups(uint8_t port, uint8_t mask) override;
    hal::Status refresh_config() override;

    // The eight shift lamps live on GPA0..GPA7, so the mask from
    // core::shift::pattern() goes straight out with no translation.
    hal::Status write_shift_lamps(uint8_t mask);

    // Pulse the panel's reset line, which is GPB6 on this part.
    hal::Status pulse_tft_reset();

    bool neutral() const { return neutral_; }
    bool card_detect() const { return card_detect_; }

    // Reads GPB and caches the neutral and card-detect bits. Neutral reads
    // LOW on the wire — opto-isolated and pulled up — and is inverted here so
    // that nothing above ever has to remember that.
    hal::Status poll_inputs();

private:
    hal::Status write_reg(uint8_t reg, uint8_t value);
    hal::Status read_reg(uint8_t reg, uint8_t& value);

    void*   dev_ = nullptr;        // spi_device_handle_t
    uint8_t dir_[2] = {0x00, 0xE0};
    uint8_t pullup_[2] = {0x00, 0xE0};
    uint8_t out_[2] = {0x00, 0x00};
    bool    neutral_ = false;
    bool    card_detect_ = false;
};

}  // namespace drivers
