// Colours and type, in one place (§3).
//
// A dark dashboard, because this is read in daylight through a visor and at
// night with dark-adapted eyes. Amber and red carry meaning and nothing else
// does — if a screen wants a fourth colour to make a point, the point needs
// making a different way.
//
// The palette itself lives in `geometry.hpp` next to `rgb()`, because a
// colour constant is useless without the type. This header is the semantic
// layer over it: widgets ask for `theme::warning`, not for orange, so that
// changing what a warning looks like is one edit.
#pragma once
#include "ui/geometry.hpp"

namespace ui::theme {

// Surfaces
inline constexpr Color background = color::kBg;
inline constexpr Color panel      = color::kPanel;
inline constexpr Color rule       = color::kGrid;

// Type
inline constexpr Color primary    = color::kText;   // the number being read
inline constexpr Color secondary  = color::kDim;    // its caption
inline constexpr Color emphasis   = color::kWhite;  // the focused row

// Meaning. These three are the only colours allowed to mean something.
inline constexpr Color nominal    = color::kGood;
inline constexpr Color warning    = color::kWarn;
inline constexpr Color alarm      = color::kAlarm;

// Interaction
inline constexpr Color selection  = color::kAccent;
inline constexpr Color trace      = color::kAccent;

// Type scale. There is one font; everything is an integer multiple of it,
// because a bitmap font scaled by 1.5 looks like a fault in the panel.
inline constexpr uint8_t caption = 1;
inline constexpr uint8_t body    = 2;
inline constexpr uint8_t heading = 3;
inline constexpr uint8_t large   = 5;
inline constexpr uint8_t hero    = 9;   // the speed a rider reads at a glance

// What a shift lamp looks like at each position along the strip: green
// through amber to red, in the same order as the lamps on the handlebar, so
// the screen and the bike never disagree.
inline constexpr Color shift_lamp(int index) {
    return index >= 5 ? alarm : (index >= 3 ? warning : nominal);
}

}  // namespace ui::theme
