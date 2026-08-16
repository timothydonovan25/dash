// A Canvas over a plain RGB565 buffer.
//
// No ESP headers: this is the same code on the device and on the host. On
// the device the buffer is a band in PSRAM that the render task hands to the
// panel driver; in the tests it is an array, which is how a screen's layout
// can be asserted without a screen.
#pragma once
#include "ui/canvas.hpp"

namespace ui {

class FrameBufferCanvas final : public Canvas {
public:
    // Does not own the buffer. `stride` is in pixels, and defaults to `w`.
    FrameBufferCanvas(Color* pixels, int16_t w, int16_t h, int16_t stride = 0)
        : px_(pixels), w_(w), h_(h), stride_(stride > 0 ? stride : w) {}

    int16_t width() const override { return w_; }
    int16_t height() const override { return h_; }

    void fill(Color c) override;
    void fill_rect(const Rect& r, Color c) override;
    void draw_rect(const Rect& r, Color c) override;
    void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) override;
    void fill_circle(int16_t cx, int16_t cy, int16_t radius, Color c) override;
    void draw_circle(int16_t cx, int16_t cy, int16_t radius, Color c) override;
    void draw_text(int16_t x, int16_t y, const char* text, Color c,
                   uint8_t scale, Align align) override;

    // The band this canvas represents within the full screen. The render
    // task sets it so a screen can draw in whole-screen coordinates while
    // only one band exists in memory (§7.6: a full frame is 460 kB).
    void set_origin(int16_t x, int16_t y) { ox_ = x; oy_ = y; }

    Color pixel(int16_t x, int16_t y) const;

private:
    void put(int16_t x, int16_t y, Color c);

    Color*  px_;
    int16_t w_;
    int16_t h_;
    int16_t stride_;
    int16_t ox_ = 0;
    int16_t oy_ = 0;
};

}  // namespace ui
