// ILI9488 on SPI3, plus the backlight.
//
// Three things about this panel drive the whole design:
//
//  - **It does not accept RGB565 over 4-wire SPI.** It needs 18-bit colour,
//    three bytes per pixel. The UI thinks in RGB565 everywhere and the
//    conversion happens here, on the way out.
//  - **A full frame is 460 kB** at that depth, so drawing happens in bands.
//  - **Its reset line is on the MCP23S17 (GPB6)**, not a GPIO, so the
//    expander has to be alive first (§6).
//
// The panel command set comes from the `esp_lcd_ili9488` managed component
// rather than being written here — see idf_component.yml. This class is the
// bridge between that and hal::IPanel.
#pragma once
#include "hal/i_display.hpp"

namespace drivers {

class Mcp23s17;

class Ili9488 final : public hal::IPanel {
public:
    explicit Ili9488(Mcp23s17* expander) : mcp_(expander) {}

    hal::Status init() override;
    uint16_t width() const override;
    uint16_t height() const override;
    hal::Status blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t* pixels) override;
    hal::Status set_backlight(uint8_t percent) override;

private:
    Mcp23s17* mcp_;
    void*     io_ = nullptr;       // esp_lcd_panel_io_handle_t
    void*     panel_ = nullptr;    // esp_lcd_panel_handle_t
    bool      backlight_ready_ = false;
};

}  // namespace drivers
