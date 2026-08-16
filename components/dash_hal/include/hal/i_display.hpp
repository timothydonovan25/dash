// The panel. Deliberately narrow: a rectangle of pixels and a backlight.
//
// The ILI9488 in 4-wire SPI mode does not accept RGB565. It needs 18-bit
// colour, three bytes per pixel, so a full 320x480 frame is 460 kB and has
// to go out in bands. That is the driver's problem — the UI thinks in one
// pixel format everywhere and never in bands.
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
