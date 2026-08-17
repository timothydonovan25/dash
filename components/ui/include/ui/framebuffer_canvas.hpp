// A Canvas over a plain RGB565 buffer.
//
// No ESP headers: this is the same code on the device and on the host. On
// the device the buffer is a band in PSRAM that the render task hands to the
// panel driver; in the tests it is an array, which is how a screen's layout
// can be asserted without a screen.
#pragma once
#include "ui/canvas.hpp"
#include <cstddef>

namespace ui {

class FrameBufferCanvas final : public Canvas {
public:
    // Does not own the buffer. `stride` is in pixels, and defaults to `w`.
    FrameBufferCanvas(Color* pixels, int16_t w, int16_t h, int16_t stride = 0)
        : px_(pixels), w_(w), h_(h), stride_(stride > 0 ? stride : w),
          capacity_(static_cast<size_t>(w) * h) {}

    int16_t width() const override { return w_; }
    int16_t height() const override { return h_; }

    void fill(Color c) override;
    void fill_rect(const Rect& r, Color c) override;
    void draw_rect(const Rect& r, Color c) override;
    void draw_round_rect(const Rect& r, int16_t radius, Color c) override;
    void fill_round_rect(const Rect& r, int16_t radius, Color c) override;
    void fill_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       int16_t x2, int16_t y2, Color c) override;
    void fill_arc(int16_t cx, int16_t cy, int16_t r_outer, int16_t r_inner,
                  float start_deg, float end_deg, Color c) override;
    void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) override;
    void fill_circle(int16_t cx, int16_t cy, int16_t radius, Color c) override;
    void draw_circle(int16_t cx, int16_t cy, int16_t radius, Color c) override;
    void draw_text(int16_t x, int16_t y, const char* text, Color c,
                   uint8_t scale, Align align) override;
    void draw_bitmap(int16_t x, int16_t y, const uint8_t* bits, int16_t w,
                     int16_t h, Color c) override;

    void set_clip(const Rect& r) override { clip_ = r; clipped_ = true; }
    void clear_clip() override { clipped_ = false; }

    // The band this canvas represents within the full screen. The render
    // task sets it so a screen can draw in whole-screen coordinates while
    // only one band exists in memory (§7.6: a full frame is 460 kB).
    void set_origin(int16_t x, int16_t y) { ox_ = x; oy_ = y; }

    // Point the buffer at an arbitrary rectangle of the screen, for updating
    // one region rather than a whole band. The same buffer is reused for
    // every region, so w*h must fit what was allocated; it silently refuses
    // rather than writing past the end.
    bool set_window(int16_t x, int16_t y, int16_t w, int16_t h) {
        if (w <= 0 || h <= 0) return false;
        if (static_cast<size_t>(w) * h > capacity_) return false;
        ox_ = x; oy_ = y; w_ = w; h_ = h; stride_ = w;
        return true;
    }

    // How many rows of a `w`-wide region fit in the buffer at once.
    int16_t rows_for(int16_t w) const {
        if (w <= 0) return 0;
        const size_t r = capacity_ / static_cast<size_t>(w);
        return r > 32767 ? 32767 : static_cast<int16_t>(r);
    }

    Color pixel(int16_t x, int16_t y) const;

private:
    void put(int16_t x, int16_t y, Color c);

    Color*  px_;
    int16_t w_;
    int16_t h_;
    int16_t stride_;
    size_t  capacity_;
    int16_t ox_ = 0;
    int16_t oy_ = 0;
    Rect    clip_{};
    bool    clipped_ = false;
};

}  // namespace ui
