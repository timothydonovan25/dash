#include "ui/framebuffer_canvas.hpp"
#include <cmath>

namespace ui {
namespace {
inline int16_t clamp16(int v, int lo, int hi) {
    return static_cast<int16_t>(v < lo ? lo : (v > hi ? hi : v));
}
}  // namespace

void FrameBufferCanvas::put(int16_t x, int16_t y, Color c) {
    if (clipped_ && !clip_.contains(x, y)) return;
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
    // The clip is applied here as well as in put(), because draw_text at any
    // scale above 1 draws its pixels as rectangles and would otherwise walk
    // straight through it.
    int16_t rx = r.x, ry = r.y, rr = r.right(), rb = r.bottom();
    if (clipped_) {
        if (rx < clip_.x) rx = clip_.x;
        if (ry < clip_.y) ry = clip_.y;
        if (rr > clip_.right()) rr = clip_.right();
        if (rb > clip_.bottom()) rb = clip_.bottom();
        if (rx >= rr || ry >= rb) return;
    }
    const int16_t x0 = clamp16(rx - ox_, 0, w_);
    const int16_t y0 = clamp16(ry - oy_, 0, h_);
    const int16_t x1 = clamp16(rr - ox_, 0, w_);
    const int16_t y1 = clamp16(rb - oy_, 0, h_);
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

namespace {
// Quarter-circle offsets: for each row of a corner, how far in the arc is.
// Computed rather than tabulated so any radius works.
inline int16_t corner_dx(int16_t radius, int16_t dy) {
    const int r2 = radius * radius - dy * dy;
    if (r2 <= 0) return radius;
    int dx = 0;
    while ((dx + 1) * (dx + 1) <= r2) ++dx;
    return static_cast<int16_t>(radius - dx);
}
}  // namespace

void FrameBufferCanvas::draw_round_rect(const Rect& r, int16_t radius, Color c) {
    if (r.empty()) return;
    const int16_t max_r = (r.w < r.h ? r.w : r.h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius <= 0) { draw_rect(r, c); return; }

    // Straight runs first, then the four corners a row at a time.
    fill_rect(Rect{static_cast<int16_t>(r.x + radius), r.y,
                   static_cast<int16_t>(r.w - 2 * radius), 1}, c);
    fill_rect(Rect{static_cast<int16_t>(r.x + radius),
                   static_cast<int16_t>(r.bottom() - 1),
                   static_cast<int16_t>(r.w - 2 * radius), 1}, c);
    fill_rect(Rect{r.x, static_cast<int16_t>(r.y + radius), 1,
                   static_cast<int16_t>(r.h - 2 * radius)}, c);
    fill_rect(Rect{static_cast<int16_t>(r.right() - 1),
                   static_cast<int16_t>(r.y + radius), 1,
                   static_cast<int16_t>(r.h - 2 * radius)}, c);

    for (int16_t dy = 0; dy < radius; ++dy) {
        const int16_t dx = corner_dx(radius, static_cast<int16_t>(radius - dy - 1));
        const int16_t top = static_cast<int16_t>(r.y + dy);
        const int16_t bot = static_cast<int16_t>(r.bottom() - 1 - dy);
        put(static_cast<int16_t>(r.x + dx), top, c);
        put(static_cast<int16_t>(r.right() - 1 - dx), top, c);
        put(static_cast<int16_t>(r.x + dx), bot, c);
        put(static_cast<int16_t>(r.right() - 1 - dx), bot, c);
    }
}

void FrameBufferCanvas::fill_round_rect(const Rect& r, int16_t radius, Color c) {
    if (r.empty()) return;
    const int16_t max_r = (r.w < r.h ? r.w : r.h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius <= 0) { fill_rect(r, c); return; }

    fill_rect(Rect{r.x, static_cast<int16_t>(r.y + radius), r.w,
                   static_cast<int16_t>(r.h - 2 * radius)}, c);
    for (int16_t dy = 0; dy < radius; ++dy) {
        const int16_t dx = corner_dx(radius, static_cast<int16_t>(radius - dy - 1));
        const int16_t w = static_cast<int16_t>(r.w - 2 * dx);
        fill_rect(Rect{static_cast<int16_t>(r.x + dx),
                       static_cast<int16_t>(r.y + dy), w, 1}, c);
        fill_rect(Rect{static_cast<int16_t>(r.x + dx),
                       static_cast<int16_t>(r.bottom() - 1 - dy), w, 1}, c);
    }
}

void FrameBufferCanvas::fill_triangle(int16_t x0, int16_t y0, int16_t x1,
                                      int16_t y1, int16_t x2, int16_t y2,
                                      Color c) {
    // Scanline fill. Used for the angled cut across the tach bar, where a
    // rectangle would look like a mistake.
    int16_t min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int16_t max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

    for (int16_t y = min_y; y <= max_y; ++y) {
        float xs[3];
        int n = 0;
        const int16_t px[3] = {x0, x1, x2};
        const int16_t py[3] = {y0, y1, y2};
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            const int16_t ya = py[i], yb = py[j];
            if ((y >= ya && y < yb) || (y >= yb && y < ya)) {
                const float t = static_cast<float>(y - ya) / static_cast<float>(yb - ya);
                xs[n++] = px[i] + t * (px[j] - px[i]);
            }
        }
        if (n < 2) continue;
        float lo = xs[0], hi = xs[1];
        if (lo > hi) { const float t = lo; lo = hi; hi = t; }
        fill_rect(Rect{static_cast<int16_t>(lo), y,
                       static_cast<int16_t>(hi - lo + 1), 1}, c);
    }
}

void FrameBufferCanvas::fill_arc(int16_t cx, int16_t cy, int16_t r_outer,
                                 int16_t r_inner, float start_deg,
                                 float end_deg, Color c) {
    if (r_outer <= 0 || end_deg <= start_deg) return;
    if (r_inner < 0) r_inner = 0;

    // Walk the bounding box and test each pixel. At gauge sizes this is a
    // few thousand pixels, which is cheaper than getting the trigonometry of
    // a scanline sweep right for arcs that cross twelve o'clock.
    const int ro2 = r_outer * r_outer;
    const int ri2 = r_inner * r_inner;
    for (int16_t dy = static_cast<int16_t>(-r_outer); dy <= r_outer; ++dy) {
        for (int16_t dx = static_cast<int16_t>(-r_outer); dx <= r_outer; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > ro2 || d2 < ri2) continue;
            // Degrees clockwise from twelve o'clock. Screen y grows down, so
            // -dy puts north at the top where a rider expects it.
            float a = std::atan2(static_cast<float>(dx),
                                 static_cast<float>(-dy)) * 57.2957795f;
            if (a < 0.0f) a += 360.0f;
            if (a < start_deg || a > end_deg) {
                if (!(start_deg < 0.0f && a - 360.0f >= start_deg &&
                      a - 360.0f <= end_deg)) {
                    continue;
                }
            }
            put(static_cast<int16_t>(cx + dx), static_cast<int16_t>(cy + dy), c);
        }
    }
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

void FrameBufferCanvas::draw_bitmap(int16_t x, int16_t y, const uint8_t* bits,
                                    int16_t w, int16_t h, Color c) {
    if (!bits || w <= 0 || h <= 0) return;
    // Rows are byte-aligned, so a 13-wide icon still steps two bytes a row.
    const int16_t stride = static_cast<int16_t>((w + 7) / 8);
    for (int16_t row = 0; row < h; ++row) {
        const uint8_t* line = bits + static_cast<size_t>(row) * stride;
        for (int16_t col = 0; col < w; ++col) {
            if (line[col >> 3] & (0x80u >> (col & 7))) {
                put(static_cast<int16_t>(x + col),
                    static_cast<int16_t>(y + row), c);
            }
        }
    }
}

}  // namespace ui
