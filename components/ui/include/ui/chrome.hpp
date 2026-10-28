// Shared furniture: the frames, captions and rules every screen draws.
//
// Inline because it is a handful of calls to Canvas and every screen wants
// it — a .cpp here would buy nothing and cost an include.
//
// The point is consistency. Fifteen screens each inventing their own idea of
// what a labelled box looks like is how a dashboard ends up feeling like
// fifteen dashboards, and the rider has to relearn the layout every time
// they press B.
#pragma once
#include <cstdio>

#include "core/hr/hr.hpp"
#include "ui/canvas.hpp"
#include "ui/theme.hpp"

namespace ui::chrome {

inline constexpr int16_t kRadius   = 6;
inline constexpr int16_t kCapGap   = 3;    // caption baseline to tile top
inline constexpr int16_t kTitleY   = 6;
inline constexpr int16_t kBodyTop  = 30;   // first row below the title rule

// Where the rider is, top left, with a rule under it: "DIAG/SENSORS".
//
// Both halves, because either alone is ambiguous. Five of the screen names
// repeat across modes or read the same at a glance, and after two presses of
// B in the dark the question is not only which screen but which mode — A and
// B are next to each other on the pod.
//
// The mode is drawn dim and the screen bright, so the eye lands on the half
// that changed most recently.
inline void title(Canvas& c, const char* mode, const char* screen) {
    char lead[24];
    size_t k = 0;
    for (const char* p = mode; *p && k + 2 < sizeof lead; ++p) lead[k++] = *p;
    lead[k++] = '/';
    lead[k] = '\0';

    c.draw_text(14, kTitleY, lead, theme::secondary, 1, Align::Left);
    c.draw_text(static_cast<int16_t>(14 + c.text_width(lead, 1)), kTitleY,
                screen, theme::primary, 1, Align::Left);
    c.fill_rect(Rect{14, 22, static_cast<int16_t>(kScreenW - 28), 1},
                theme::rule);
}

// A framed box with its caption sitting above the frame, so the number
// inside gets the whole tile.
inline void tile(Canvas& c, const Rect& r, const char* caption) {
    c.draw_round_rect(r, kRadius, theme::rule);
    if (caption && caption[0]) {
        c.draw_text(static_cast<int16_t>(r.x + 4),
                    static_cast<int16_t>(r.y - 9 - kCapGap + 9), caption,
                    theme::secondary, 1, Align::Left);
    }
}

// Caption above the tile rather than inside it, for rows of small tiles
// where the frame would crowd the text.
inline void tile_above(Canvas& c, const Rect& r, const char* caption) {
    c.draw_round_rect(r, kRadius, theme::rule);
    if (caption && caption[0]) {
        c.draw_text(r.x, static_cast<int16_t>(r.y - 10), caption,
                    theme::secondary, 1, Align::Left);
    }
}

// A unit label tucked under the right edge of a tile.
inline void unit(Canvas& c, const Rect& r, const char* u) {
    if (!u || !u[0]) return;
    c.draw_text(static_cast<int16_t>(r.right() - 4),
                static_cast<int16_t>(r.bottom() + 3), u, theme::secondary, 1,
                Align::Right);
}

// The rider's heart rate is coloured by zone wherever it shows (HEART, SPEED,
// RACE), so a colour means the same effort on every screen.
inline Color hr_zone_color(uint8_t zone) {
    switch (zone) {
        case 1:  return theme::secondary;
        case 2:  return color::kAccent;
        case 3:  return color::kGood;
        case 4:  return color::kWarn;
        case 5:  return color::kAlarm;
        default: return theme::primary;
    }
}

// SPEED's and RACE's heart rate: the bpm while a reading is current, "--"
// while Watch HR is on and none is, and nothing at all when it is off, so a
// rider without a watch never sees the space. `suffix` follows the number.
inline Color hr_readout(const core::state::Heart& h, uint32_t t_ms, char* buf, size_t n,
                        const char* suffix) {
    if (h.link == core::state::Heart::Off) {
        if (n) buf[0] = '\0';
        return color::kDim;
    }
    if (!core::hr::fresh(h, t_ms)) {
        std::snprintf(buf, n, "--%s", suffix);
        return color::kDim;
    }
    std::snprintf(buf, n, "%u%s", static_cast<unsigned>(h.bpm), suffix);
    return hr_zone_color(core::hr::zone(h.bpm, h.zone_max_bpm));
}

}  // namespace ui::chrome
