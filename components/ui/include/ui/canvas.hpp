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

// How wide a string renders, without needing a Canvas to ask. There is one
// font on this dash, so this is a property of the string and the scale and
// nothing else — which lets a widget decide whether its text fits before it
// has been handed anything to draw on.
int16_t text_width_px(const char* text, uint8_t scale);

class Canvas {
public:
    virtual ~Canvas() = default;

    virtual int16_t width() const = 0;
    virtual int16_t height() const = 0;

    // Smoothed glyphs at the small scale too (ui/font_aa.hpp's opt-in
    // sizes), until turned off again. For a widget that is read as a
    // sentence, like MEDIA's lyric; it sets this around its own drawing and
    // puts it back. A canvas without smoothed glyphs ignores it.
    virtual void set_smooth_small(bool) {}

    // CONFIG -> Display -> Palette (ui::theme::palette_color): every colour
    // drawn after this goes through that palette. The App sets it before
    // each draw; a canvas that records calls ignores it.
    virtual void set_palette(uint8_t) {}

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

    // A full-colour image: row-major RGB565, w*h entries, straight copied in
    // with no per-pixel colour substitution — album art, not an icon. Off-
    // screen rows and columns are clipped like everything else; there is no
    // scaling, because the one source of these (`ui/album_art.hpp`) is
    // baked at the exact size it is ever drawn.
    virtual void draw_image(int16_t x, int16_t y, const uint16_t* pixels,
                            int16_t w, int16_t h) = 0;

    // Confine drawing to a rectangle, in whole-screen coordinates. What this
    // is for is text that is deliberately larger than the space it is in —
    // a marquee draws its string starting off the left edge of the window,
    // and without a clip that string lands on whatever is next door.
    //
    // Does not nest: one region at a time. Use ClipGuard rather than pairing
    // the calls by hand.
    virtual void set_clip(const Rect& r) = 0;
    virtual void clear_clip() = 0;

    // Whether anything drawn inside `r` could land in this canvas. A band
    // canvas says no for everything outside its band, so a full repaint
    // (eight bands) skips the widgets a band cannot show instead of drawing
    // every widget eight times over. Conservative: true unless sure.
    virtual bool intersects(const Rect& r) const { (void)r; return true; }

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

// Sets a clip for the length of a scope. An early return out of a render
// that had set a clip by hand would leave every later widget clipped to a
// rectangle it has never heard of, and that is a bug that looks like the
// panel is broken.
class ClipGuard {
public:
    ClipGuard(Canvas& c, const Rect& r) : c_(c) { c_.set_clip(r); }
    ~ClipGuard() { c_.clear_clip(); }
    ClipGuard(const ClipGuard&) = delete;
    ClipGuard& operator=(const ClipGuard&) = delete;

private:
    Canvas& c_;
};

}  // namespace ui
