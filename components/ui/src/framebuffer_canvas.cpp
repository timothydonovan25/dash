#include "ui/framebuffer_canvas.hpp"

namespace ui {
namespace {
inline int16_t clamp16(int v, int lo, int hi) {
    return static_cast<int16_t>(v < lo ? lo : (v > hi ? hi : v));
}
}  // namespace

void FrameBufferCanvas::put(int16_t x, int16_t y, Color c) {
    const int16_t px = static_cast<int16_t>(x - ox_);
    const int16_t py = static_cast<int16_t>(y - oy_);
    // Everything clips here, once. A widget that draws two pixels outside
    // its bounds is a cosmetic bug; a widget that writes two pixels outside
    // the buffer is a reboot.
    if (px < 0 || py < 0 || px >= w_ || py >= h_) return;
    px_[py * stride_ + px] = c;
}

Color FrameBufferCanvas::pixel(int16_t x, int16_t y) const {
    const int16_t px = static_cast<int16_t>(x - ox_);
    const int16_t py = static_cast<int16_t>(y - oy_);
    if (px < 0 || py < 0 || px >= w_ || py >= h_) return 0;
    return px_[py * stride_ + px];
}

void FrameBufferCanvas::fill(Color c) {
    for (int16_t y = 0; y < h_; ++y)
        for (int16_t x = 0; x < w_; ++x) px_[y * stride_ + x] = c;
}

void FrameBufferCanvas::fill_rect(const Rect& r, Color c) {
    if (r.empty()) return;
    const int16_t x0 = clamp16(r.x - ox_, 0, w_);
    const int16_t y0 = clamp16(r.y - oy_, 0, h_);
    const int16_t x1 = clamp16(r.right() - ox_, 0, w_);
    const int16_t y1 = clamp16(r.bottom() - oy_, 0, h_);
    for (int16_t y = y0; y < y1; ++y)
        for (int16_t x = x0; x < x1; ++x) px_[y * stride_ + x] = c;
}

void FrameBufferCanvas::draw_rect(const Rect& r, Color c) {
    if (r.empty()) return;
    fill_rect(Rect{r.x, r.y, r.w, 1}, c);
    fill_rect(Rect{r.x, static_cast<int16_t>(r.bottom() - 1), r.w, 1}, c);
    fill_rect(Rect{r.x, r.y, 1, r.h}, c);
    fill_rect(Rect{static_cast<int16_t>(r.right() - 1), r.y, 1, r.h}, c);
}

void FrameBufferCanvas::draw_line(int16_t x0, int16_t y0, int16_t x1,
                                  int16_t y1, Color c) {
    // Bresenham. Integer only — there is no FPU time to spare in a render
    // pass that redraws a track map.
    int dx = x1 - x0, dy = y1 - y0;
    const int sx = dx >= 0 ? 1 : -1;
    const int sy = dy >= 0 ? 1 : -1;
    dx = dx >= 0 ? dx : -dx;
    dy = dy >= 0 ? dy : -dy;
    int err = dx - dy;
    for (;;) {
        put(static_cast<int16_t>(x0), static_cast<int16_t>(y0), c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 = static_cast<int16_t>(x0 + sx); }
        if (e2 <  dx) { err += dx; y0 = static_cast<int16_t>(y0 + sy); }
    }
}

void FrameBufferCanvas::draw_circle(int16_t cx, int16_t cy, int16_t radius,
                                    Color c) {
    if (radius <= 0) return;
    int x = radius, y = 0, err = 1 - radius;
    while (x >= y) {
        put(static_cast<int16_t>(cx + x), static_cast<int16_t>(cy + y), c);
        put(static_cast<int16_t>(cx + y), static_cast<int16_t>(cy + x), c);
        put(static_cast<int16_t>(cx - y), static_cast<int16_t>(cy + x), c);
        put(static_cast<int16_t>(cx - x), static_cast<int16_t>(cy + y), c);
        put(static_cast<int16_t>(cx - x), static_cast<int16_t>(cy - y), c);
        put(static_cast<int16_t>(cx - y), static_cast<int16_t>(cy - x), c);
        put(static_cast<int16_t>(cx + y), static_cast<int16_t>(cy - x), c);
        put(static_cast<int16_t>(cx + x), static_cast<int16_t>(cy - y), c);
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}

void FrameBufferCanvas::fill_circle(int16_t cx, int16_t cy, int16_t radius,
                                    Color c) {
    if (radius <= 0) return;
    for (int16_t dy = static_cast<int16_t>(-radius); dy <= radius; ++dy) {
        const int span = radius * radius - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= span) ++dx;
        fill_rect(Rect{static_cast<int16_t>(cx - dx),
                       static_cast<int16_t>(cy + dy),
                       static_cast<int16_t>(2 * dx + 1), 1}, c);
    }
}

void FrameBufferCanvas::draw_text(int16_t x, int16_t y, const char* text,
                                  Color c, uint8_t scale, Align align) {
    if (!text || !*text) return;
    if (scale == 0) scale = 1;
    const Font& f = font_5x7();

    const int16_t w = text_width(text, scale);
    int16_t pen = x;
    if (align == Align::Center) pen = static_cast<int16_t>(x - w / 2);
    else if (align == Align::Right) pen = static_cast<int16_t>(x - w);

    for (const char* ch = text; *ch; ++ch) {
        const uint8_t code = static_cast<uint8_t>(*ch);
        if (code >= f.first && code <= f.last) {
            const uint8_t* glyph = f.glyphs + (code - f.first) * f.width;
            for (uint8_t col = 0; col < f.width; ++col) {
                const uint8_t bits = glyph[col];
                for (uint8_t row = 0; row < f.height; ++row) {
                    if (!(bits & (1u << row))) continue;
                    if (scale == 1) {
                        put(static_cast<int16_t>(pen + col),
                            static_cast<int16_t>(y + row), c);
                    } else {
                        fill_rect(Rect{static_cast<int16_t>(pen + col * scale),
                                       static_cast<int16_t>(y + row * scale),
                                       scale, scale}, c);
                    }
                }
            }
        }
        pen = static_cast<int16_t>(pen + f.advance * scale);
    }
}

}  // namespace ui
