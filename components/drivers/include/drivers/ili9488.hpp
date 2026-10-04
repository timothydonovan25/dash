// ILI9488 on the 8-bit 8080 bus, plus the backlight.
//
// Three things about this panel and this bus drive the design:
//
//  - **RGB565 goes out as is**, two bytes per pixel. 8080 mode accepts it;
//    only SPI mode needed the RGB666 conversion. The I80 peripheral swaps
//    each pixel's bytes on the way out, because the panel wants the high
//    byte first and the UI's pixels are little-endian in memory.
//  - **The P4's DMA wants every transfer's address and length on a 32-byte
//    boundary.** The UI blits rectangles of any size, so pixels are staged
//    through an aligned buffer owned here, and any tail shorter than 32
//    bytes goes out as the parameters of a Memory Write Continue command.
//    The caller's buffer is free the moment blit() returns.
//  - **The backlight is on the PCA9955B**, so set_backlight() forwards there.
//
// The panel command set comes from the `esp_lcd_ili9488` managed component.
// This class bridges that to hal::IPanel and owns the blit path itself.
#pragma once
#include "hal/i_display.hpp"

namespace drivers {

class Pca9955b;

class Ili9488 final : public hal::IPanel {
public:
    // `lcd_bus` is bsp::BusStatus::lcd_bus. Either may be null, and the
    // panel then reports NotPresent rather than crashing.
    Ili9488(void* lcd_bus, Pca9955b* backlight)
        : bus_(lcd_bus), backlight_(backlight) {}

    hal::Status init() override;
    uint16_t width() const override;
    uint16_t height() const override;
    hal::Status blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t* pixels) override;
    hal::Status set_backlight(uint8_t percent) override;

private:
    void*     bus_;
    Pca9955b* backlight_;
    void*     io_ = nullptr;        // esp_lcd_panel_io_handle_t
    void*     panel_ = nullptr;     // esp_lcd_panel_handle_t
    uint8_t*  staging_ = nullptr;   // DMA-capable, 64-byte aligned
};

}  // namespace drivers
