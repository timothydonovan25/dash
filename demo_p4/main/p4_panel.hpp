// The demo board's panel: an ST7701-family MIPI-DSI IPS panel, portrait
// 480x800 native, physically rotated 90 degrees into a landscape case.
//
// UNPROVEN — see docs/bringup-p4-demo.md. This dropship board has no
// published schematic, so the controller IC, its init sequence, and every
// pin in p4_pins.hpp are a best guess for this board class rather than a
// verified fact the way components/bsp/pins.hpp is for the real dash.
//
// Bridges hal::IPanel, same contract drivers::Ili9488 implements for the
// real board (components/drivers/include/drivers/ili9488.hpp): the UI hands
// over RGB565 in its own 480x320 landscape coordinates, and this class is
// responsible for getting those pixels onto the actual screen, wherever
// they land in the panel's native, rotated, framebuffer.
#pragma once
#include "hal/i_display.hpp"

namespace demo {

class P4Panel final : public hal::IPanel {
public:
    hal::Status init() override;
    uint16_t width() const override;
    uint16_t height() const override;
    hal::Status blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t* pixels) override;
    hal::Status set_backlight(uint8_t percent) override;

    // The whole 800x480 view, not the dash's 480x320 canvas scaled 1.5x into
    // it: for a program that takes the panel over (DIAG -> Doom). Draws a
    // w x h RGB565 image with its top-left at view (x, y), each pixel a
    // scale x scale block. Unsupported if any of it would fall off the view.
    hal::Status blit_view(int x, int y, int w, int h, const uint16_t* pixels, int scale);

private:
    void*    dsi_bus_ = nullptr;    // esp_lcd_dsi_bus_handle_t
    void*    io_      = nullptr;    // esp_lcd_panel_io_handle_t
    void*    panel_   = nullptr;    // esp_lcd_panel_handle_t
    uint16_t* fb_      = nullptr;   // native framebuffer, 480(w) x 800(h)
    // blit()'s source row for each destination row (the 480-row view).
    uint16_t  src_row_[480] = {};
    bool     backlight_gpio_only_ = true;   // true until LEDC dimming configures
};

}  // namespace demo
