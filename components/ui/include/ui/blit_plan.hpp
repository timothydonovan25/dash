// How a widget's rectangle becomes panel transfers.
//
// `render` has no framebuffer — it has one band buffer — so a region goes
// out as one or more horizontal strips that fit inside it. That split is
// what decides how many transfers a frame costs, and on this panel the
// transfer count and the pixel count are the whole frame budget (§7.6).
//
// It lives here, on its own, because two things need the answer: the render
// task, which does the blitting, and `ui/wire_cost.hpp`, which prices it. A
// cost model carrying its own copy of this arithmetic would agree with the
// render task exactly until somebody changed one of them, and then it would
// keep reporting confidently.
#pragma once
#include <cstddef>
#include "ui/geometry.hpp"

namespace ui {

// The most strips one region can become. A full-width widget at a 40-line
// band is eight; the margin is so callers can put the array on the stack
// and stop thinking about it.
inline constexpr int kMaxStrips = 16;

// How many rows of a `w`-wide region fit in a buffer of `capacity` pixels.
inline int16_t rows_for(int16_t w, size_t capacity) {
    if (w <= 0) return 0;
    const size_t r = capacity / static_cast<size_t>(w);
    return r > 32767 ? 32767 : static_cast<int16_t>(r);
}

// Result of planning: how many strips, or why there are none.
enum class PlanResult : int {
    Offscreen = 0,   // nothing to send; not an error
    RowTooWide = -1, // a single row will not fit the band buffer
    TooManyStrips = -2,
};

// Clips `r` to the screen and splits it into strips that fit `band_pixels`.
// Returns the strip count written to `out`, or a negative PlanResult.
inline int plan_strips(Rect r, size_t band_pixels, Rect* out, int max_out) {
    // The same clip the render task applies before it touches the panel. A
    // widget may be laid out partly off-screen; the ILI9488 will accept a
    // window running past its own edge and then wrap it.
    if (r.x < 0) { r.w = static_cast<int16_t>(r.w + r.x); r.x = 0; }
    if (r.y < 0) { r.h = static_cast<int16_t>(r.h + r.y); r.y = 0; }
    if (r.right()  > kScreenW) r.w = static_cast<int16_t>(kScreenW - r.x);
    if (r.bottom() > kScreenH) r.h = static_cast<int16_t>(kScreenH - r.y);
    if (r.empty()) return static_cast<int>(PlanResult::Offscreen);

    const int16_t rows = rows_for(r.w, band_pixels);
    if (rows <= 0) return static_cast<int>(PlanResult::RowTooWide);

    int n = 0;
    for (int16_t y = r.y; y < r.bottom(); y = static_cast<int16_t>(y + rows)) {
        if (n >= max_out) return static_cast<int>(PlanResult::TooManyStrips);
        const int16_t h = static_cast<int16_t>(
            y + rows > r.bottom() ? r.bottom() - y : rows);
        out[n++] = Rect{r.x, y, r.w, h};
    }
    return n;
}

}  // namespace ui
