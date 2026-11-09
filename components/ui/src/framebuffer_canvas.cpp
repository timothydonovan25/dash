#include "ui/framebuffer_canvas.hpp"
#include "ui/font_aa.hpp"
#include "ui/theme.hpp"
#include <cmath>

namespace ui {
namespace {
inline int16_t clamp16(int v, int lo, int hi) {
    return static_cast<int16_t>(v < lo ? lo : (v > hi ? hi : v));
}

// `fg` over `bg` at coverage a/15, a channel at a time in RGB565. Straight
// in 565, not linear light: at these sizes the difference is a shade of one
// edge pixel, and it costs nothing to leave out.
inline Color blend565(Color fg, Color bg, uint32_t a) {
    const uint32_t na = 15 - a;
    const uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * na + 7) / 15;
    const uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * na + 7) / 15;
    const uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * na + 7) / 15;
    return static_cast<Color>((r << 11) | (g << 5) | b);
}
}  // namespace

void FrameBufferCanvas::draw_aa_glyph(const AaFont& font, const AaGlyph& g, int16_t cx,
                                      int16_t cy, Color c, const Rect& vis) {
    if (g.w == 0 || g.h == 0) return;
    const Rect ink{static_cast<int16_t>(cx + g.x), static_cast<int16_t>(cy + g.y),
                   g.w, g.h};
    const Rect r = ink.clipped_to(vis);
    if (r.empty()) return;
    const int16_t row_bytes = static_cast<int16_t>((g.w + 1) / 2);
    const uint8_t* data = font.data + g.offset;
    for (int16_t y = r.y; y < r.bottom(); ++y) {
        const uint8_t* row = data + (y - ink.y) * row_bytes;
        Color* out = px_ + (y - oy_) * stride_ - ox_;
        for (int16_t x = r.x; x < r.right(); ++x) {
            const int16_t i = static_cast<int16_t>(x - ink.x);
            const uint8_t b = row[i >> 1];
            const uint32_t a = (i & 1) ? (b & 0x0F) : (b >> 4);
            if (a == 0) continue;
            out[x] = a == 15 ? c : blend565(c, out[x], a);
        }
    }
}

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
    c = theme::palette_color(c, palette_);
    for (int16_t y = 0; y < h_; ++y)
        for (int16_t x = 0; x < w_; ++x) px_[y * stride_ + x] = c;
}

void FrameBufferCanvas::fill_rect(const Rect& r, Color c) {
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
    if (r_outer <= 0 || end_deg <= start_deg) return;
    if (r_inner < 0) r_inner = 0;

    // Only the rows and columns that can land: the window intersected with
    // the clip. The old version walked the whole bounding box and called
    // atan2 for every pixel of it, for every strip a widget was drawn in --
    // the lean gauge alone was ~45k atan2 a render, and a full repaint of the
    // speed screen ~15 ms on a desktop CPU. Sides of the sector are now
    // tested with cross products against its two edge directions.
    int vx0 = ox_, vy0 = oy_, vx1 = ox_ + w_, vy1 = oy_ + h_;
    if (clipped_) {
        if (clip_.x > vx0) vx0 = clip_.x;
        if (clip_.y > vy0) vy0 = clip_.y;
        if (clip_.right() < vx1) vx1 = clip_.right();
        if (clip_.bottom() < vy1) vy1 = clip_.bottom();
    }
    const int y_lo = cy - r_outer > vy0 ? cy - r_outer : vy0;
    const int y_hi = cy + r_outer + 1 < vy1 ? cy + r_outer + 1 : vy1;
    const int x_lo = cx - r_outer > vx0 ? cx - r_outer : vx0;
    const int x_hi = cx + r_outer + 1 < vx1 ? cx + r_outer + 1 : vx1;
    if (y_lo >= y_hi || x_lo >= x_hi) return;

    // Also handles a sector crossing north from the left (end_deg > 360),
    // which the old wrap test missed: the lean marker lost its right half
    // between -7 and 0 degrees.
    //
    // Angles are degrees clockwise from north; a pixel offset (dx, dy) is the
    // vector v = (dx, -dy) in (east, north). With u = (sin a, cos a),
    // cross(u, v) = u.x*v.y - u.y*v.x is <= 0 when v is clockwise of u.
    // sin/cos of a clockwise-from-north angle. Multiples of 45 degrees are
    // snapped to exact values so pixels exactly on an axis or diagonal ray
    // are decided by exact zeros, not by which side float rounding fell on.
    struct Dir { float x, y; };
    const auto dir = [](float deg) -> Dir {
        constexpr float kR = 0.70710678118654752f;
        static const float kS[8] = {0, kR, 1, kR, 0, -kR, -1, -kR};
        static const float kC[8] = {1, kR, 0, -kR, -1, -kR, 0, kR};
        const float q = deg / 45.0f;
        const float fq = std::floor(q);
        if (q == fq) {
            int k = static_cast<int>(fq) % 8;
            if (k < 0) k += 8;
            return Dir{kS[k], kC[k]};
        }
        constexpr float kDegToRad = 0.017453292519943295f;
        return Dir{std::sin(deg * kDegToRad), std::cos(deg * kDegToRad)};
    };
    const Dir ds = dir(start_deg), de = dir(end_deg);
    const float sx = ds.x, sy = ds.y, ex = de.x, ey = de.y;
    const bool full = end_deg - start_deg >= 360.0f;
    const bool reflex = end_deg - start_deg > 180.0f;

    const int ro2 = r_outer * r_outer;
    const int ri2 = r_inner * r_inner;
    for (int y = y_lo; y < y_hi; ++y) {
        const int dy = y - cy;
        const int rem = ro2 - dy * dy;               // > 0 for |dy| <= r_outer
        if (rem < 0) continue;
        int xo = static_cast<int>(std::sqrt(static_cast<float>(rem)));
        while ((xo + 1) * (xo + 1) <= rem) ++xo;     // exact integer floor sqrt
        while (xo * xo > rem) --xo;
        const int a = cx - xo > x_lo ? cx - xo : x_lo;
        const int b = cx + xo + 1 < x_hi ? cx + xo + 1 : x_hi;
        const float vy = static_cast<float>(-dy);
        Color* row = px_ + (y - oy_) * stride_ - ox_;
        for (int x = a; x < b; ++x) {
            const int dx = x - cx;
            if (dx * dx + dy * dy < ri2) continue;
            if (!full) {
                const float vx = static_cast<float>(dx);
                const float cs = sx * vy - sy * vx;
                const float ce = ex * vy - ey * vx;
                const bool in = reflex ? !(cs > 0.0f && ce < 0.0f)
                                       : (cs <= 0.0f && ce >= 0.0f);
                if (!in) continue;
            }
            row[x] = c;
        }
    }
}

void FrameBufferCanvas::draw_line(int16_t x0, int16_t y0, int16_t x1,
                                  int16_t y1, Color c) {
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
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
    c = theme::palette_color(c, palette_);
    if (!text || !*text) return;
    if (scale == 0) scale = 1;
    const Font& f = font_5x7();

    const int16_t w = text_width(text, scale);
    int16_t pen = x;
    if (align == Align::Center) pen = static_cast<int16_t>(x - w / 2);
    else if (align == Align::Right) pen = static_cast<int16_t>(x - w);

    // A glyph wholly outside the window (or the clip) is skipped whole,
    // rather than as 35 pixel tests that each find nothing to draw: a strip
    // of a big number or a table goes past most of the text in the widget.
    Rect vis{ox_, oy_, w_, h_};
    if (clipped_) vis = vis.clipped_to(clip_);
    const int16_t gw = static_cast<int16_t>(f.width * scale);
    const int16_t gh = static_cast<int16_t>(f.height * scale);
    if (!vis.intersects(Rect{pen, y, static_cast<int16_t>(w > 0 ? w : 1), gh})) return;

    // The large sizes have smoothed glyphs in the same cells (ui/font_aa.hpp).
    // A character that set leaves out is drawn from the bitmap.
    const AaFont* aa = antialias_ ? aa_font(scale, smooth_small_) : nullptr;

    for (const char* ch = text; *ch; ++ch) {
        const uint8_t code = static_cast<uint8_t>(*ch);
        const AaGlyph* ag = aa ? aa->find(code) : nullptr;
        if (ag) {
            if (vis.intersects(Rect{pen, y, gw, gh})) draw_aa_glyph(*aa, *ag, pen, y, c, vis);
        } else if (code >= f.first && code <= f.last &&
            vis.intersects(Rect{pen, y, gw, gh})) {
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
    c = theme::palette_color(c, palette_);
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

void FrameBufferCanvas::draw_image(int16_t x, int16_t y, const uint16_t* pixels,
                                   int16_t w, int16_t h) {
    if (!pixels || w <= 0 || h <= 0) return;
    for (int16_t row = 0; row < h; ++row) {
        const uint16_t* line = pixels + static_cast<size_t>(row) * w;
        for (int16_t col = 0; col < w; ++col) {
            put(static_cast<int16_t>(x + col), static_cast<int16_t>(y + row),
                line[col]);
        }
    }
}

}  // namespace ui
