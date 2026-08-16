// What a widget is allowed to do.
//
// Abstract on purpose. The device implementation writes RGB565 into a band
// buffer and hands it to the panel driver; the host tests use a canvas that
// records what was asked for. That is what makes "does the lap screen show
// the right time" a unit test instead of a ride.
#pragma once
#include <cstdint>
#include "ui/geometry.hpp"

namespace ui {

// A monospaced bitmap font, column-major, one byte per column, bit 0 at the
// top. Small enough to keep in flash next to the code that uses it.
struct Font {
    const uint8_t* glyphs;   // (last - first + 1) * width bytes
    uint8_t        width;    // pixels of ink per glyph
    uint8_t        height;
    uint8_t        first;    // first ASCII code in the table
    uint8_t        last;
    uint8_t        advance;  // width + inter-character gap
};

const Font& font_5x7();

class Canvas {
public:
    virtual ~Canvas() = default;

    virtual int16_t width() const = 0;
    virtual int16_t height() const = 0;

    virtual void fill(Color c) = 0;
    virtual void fill_rect(const Rect& r, Color c) = 0;
    virtual void draw_rect(const Rect& r, Color c) = 0;
    virtual void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) = 0;
    virtual void fill_circle(int16_t cx, int16_t cy, int16_t radius, Color c) = 0;
    virtual void draw_circle(int16_t cx, int16_t cy, int16_t radius, Color c) = 0;

    // `x` is the left, centre or right edge according to `align`; `y` is the
    // top. Scale is an integer multiplier — there is one font on this dash
    // and everything else is a multiple of it.
    virtual void draw_text(int16_t x, int16_t y, const char* text, Color c,
                           uint8_t scale = 1, Align align = Align::Left) = 0;

    int16_t text_width(const char* text, uint8_t scale = 1) const;
    int16_t text_height(uint8_t scale = 1) const;

    // Convenience shared by every implementation.
    void draw_hline(int16_t x, int16_t y, int16_t w, Color c) {
        fill_rect(Rect{x, y, w, 1}, c);
    }
    void draw_vline(int16_t x, int16_t y, int16_t h, Color c) {
        fill_rect(Rect{x, y, 1, h}, c);
    }
};

}  // namespace ui
