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

    // Rounded rectangles are the frame around every readout on this dash,
    // so they are a primitive rather than something each screen draws.
    virtual void draw_round_rect(const Rect& r, int16_t radius, Color c) = 0;
    virtual void fill_round_rect(const Rect& r, int16_t radius, Color c) = 0;

    virtual void fill_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               int16_t x2, int16_t y2, Color c) = 0;

    // A filled annulus sector: the lean gauge and the tach arc are both this.
    //
    // Angles are degrees clockwise from twelve o'clock, which is how a rider
    // reads a gauge and how core/geo already talks about headings. `r_outer`
    // is the outside edge; `r_inner` makes it a band rather than a pie slice.
    virtual void fill_arc(int16_t cx, int16_t cy, int16_t r_outer,
                          int16_t r_inner, float start_deg, float end_deg,
                          Color c) = 0;
    virtual void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) = 0;
    virtual void fill_circle(int16_t cx, int16_t cy, int16_t radius, Color c) = 0;
    virtual void draw_circle(int16_t cx, int16_t cy, int16_t radius, Color c) = 0;

    // `x` is the left, centre or right edge according to `align`; `y` is the
    // top. Scale is an integer multiplier — there is one font on this dash
    // and everything else is a multiple of it.
    virtual void draw_text(int16_t x, int16_t y, const char* text, Color c,
                           uint8_t scale = 1, Align align = Align::Left) = 0;

    // A 1-bit-per-pixel image: row-major, MSB first, each row padded out to a
    // whole byte. Set bits are drawn in `c` and clear bits are left alone, so
    // an icon lands on whatever is already there.
    //
    // This is the format every bitmap converter emits, which is the point —
    // an icon comes out of a tool as a byte array and goes in as one, with
    // nothing in between to get wrong.
    virtual void draw_bitmap(int16_t x, int16_t y, const uint8_t* bits,
                             int16_t w, int16_t h, Color c) = 0;

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
