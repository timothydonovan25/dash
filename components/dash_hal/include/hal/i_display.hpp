// The panel. Deliberately narrow: a rectangle of pixels and a backlight.
//
// On the P4 board the ILI9488 is on an 8-bit 8080 bus and takes RGB565
// directly, two bytes per pixel. The bus DMA has alignment rules the UI
// should not have to know about, and the backlight is on the LED driver,
// not on a GPIO. Both are the driver's problem — the UI thinks in one pixel
// format everywhere and never in bus transfers.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IPanel {
public:
    virtual ~IPanel() = default;
    virtual Status init() = 0;

    virtual uint16_t width() const = 0;
    virtual uint16_t height() const = 0;

    // RGB565 in, whatever the panel wants out.
    virtual Status blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint16_t* pixels) = 0;

    virtual Status set_backlight(uint8_t percent) = 0;
};

}  // namespace hal
