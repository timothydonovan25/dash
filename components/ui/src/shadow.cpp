#include "ui/shadow.hpp"
#include <cstring>

namespace ui {
namespace {

bool on_screen(const Rect& r) {
    return !r.empty() && r.x >= 0 && r.y >= 0 && r.right() <= kScreenW &&
           r.bottom() <= kScreenH;
}

Color* row_of(Color* store, int16_t x, int16_t y) {
    return store + static_cast<size_t>(y) * kScreenW + x;
}

}  // namespace

void Shadow::record(const Rect& r, const Color* px) {
    if (!px_ || !on_screen(r)) return;
    for (int16_t row = 0; row < r.h; ++row) {
        std::memcpy(row_of(px_, r.x, static_cast<int16_t>(r.y + row)),
                    px + static_cast<size_t>(row) * r.w, r.w * sizeof(Color));
    }
}

int Shadow::diff(const Rect& r, const Color* px, Rect* out, int max_out,
                 uint32_t overhead_px) {
    if (r.empty() || max_out <= 0) return 0;
    // Nothing to compare against: all of it goes, and is remembered.
    if (!valid_ || !on_screen(r)) {
        record(r, px);
        out[0] = r;
        return 1;
    }

    int n = 0;
    bool open = false;
    Rect cur{};
    for (int16_t row = 0; row < r.h; ++row) {
        const Color* src = px + static_cast<size_t>(row) * r.w;
        Color* dst = row_of(px_, r.x, static_cast<int16_t>(r.y + row));

        // The first and last pixel that differ; a row that matches is skipped.
        int16_t a = 0;
        while (a < r.w && src[a] == dst[a]) ++a;
        if (a == r.w) continue;
        int16_t b = r.w;
        while (src[b - 1] == dst[b - 1]) --b;
        std::memcpy(dst + a, src + a, static_cast<size_t>(b - a) * sizeof(Color));

        const int16_t x0 = static_cast<int16_t>(r.x + a);
        const int16_t x1 = static_cast<int16_t>(r.x + b);
        const int16_t y = static_cast<int16_t>(r.y + row);
        if (!open) {
            cur = Rect{x0, y, static_cast<int16_t>(x1 - x0), 1};
            open = true;
            continue;
        }

        // Grow the open part down to this row, or close it and start another:
        // whichever puts fewer pixels' worth on the wire. Growing sends the
        // unchanged rows and columns in between, which the panel already has
        // and does not mind getting again.
        const int16_t ux0 = cur.x < x0 ? cur.x : x0;
        const int16_t ux1 = cur.right() > x1 ? cur.right() : x1;
        const int16_t uh = static_cast<int16_t>(y + 1 - cur.y);
        const uint32_t merged = static_cast<uint32_t>(ux1 - ux0) * uh;
        const uint32_t split = static_cast<uint32_t>(cur.w) * cur.h +
                               static_cast<uint32_t>(x1 - x0) + overhead_px;
        if (merged <= split || n == max_out - 1) {
            cur = Rect{ux0, cur.y, static_cast<int16_t>(ux1 - ux0), uh};
        } else {
            out[n++] = cur;
            cur = Rect{x0, y, static_cast<int16_t>(x1 - x0), 1};
        }
    }
    if (open) out[n++] = cur;
    return n;
}

void Shadow::pack(const Rect& r, Color* px, const Rect& part) {
    // Forward, row by row: each destination row starts no later than its
    // source row (part.w <= r.w), so nothing is read after it is overwritten.
    for (int16_t row = 0; row < part.h; ++row) {
        const Color* src = px + static_cast<size_t>(part.y - r.y + row) * r.w +
                           (part.x - r.x);
        std::memmove(px + static_cast<size_t>(row) * part.w, src,
                     static_cast<size_t>(part.w) * sizeof(Color));
    }
}

}  // namespace ui
