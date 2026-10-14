// Rectangles and colours. No hardware, no allocation.
#pragma once
#include <cstdint>

namespace ui {

// RGB565, because that is what the UI thinks in everywhere, and what the
// ILI9488 takes as is on the P4 board's 8-bit 8080 bus (§7.6).
using Color = uint16_t;

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<Color>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// A dark dashboard, because this thing is read in daylight through a visor
// and at night with dark-adapted eyes. Amber and red carry meaning; nothing
// else does.
namespace color {
constexpr Color kBlack    = rgb(0, 0, 0);
constexpr Color kBg       = rgb(8, 10, 14);
constexpr Color kPanel    = rgb(22, 26, 34);
constexpr Color kGrid     = rgb(44, 52, 64);
constexpr Color kText     = rgb(226, 232, 240);
constexpr Color kDim      = rgb(120, 132, 150);
constexpr Color kAccent   = rgb(56, 170, 255);
constexpr Color kGood     = rgb(64, 200, 120);
constexpr Color kWarn     = rgb(255, 176, 32);
constexpr Color kAlarm    = rgb(255, 64, 56);
// The tach bar red, matched to the reference layout: RGB565 0xE945.
constexpr Color kTach     = rgb(239, 40, 41);
// The unlit part of the tach bar: dark enough to read as empty, not invisible.
constexpr Color kTachTrack= rgb(72, 14, 14);
constexpr Color kWhite    = rgb(255, 255, 255);
}  // namespace color

struct Point {
    int16_t x = 0;
    int16_t y = 0;
};

struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;

    constexpr int16_t right()  const { return static_cast<int16_t>(x + w); }
    constexpr int16_t bottom() const { return static_cast<int16_t>(y + h); }
    constexpr int16_t cx()     const { return static_cast<int16_t>(x + w / 2); }
    constexpr int16_t cy()     const { return static_cast<int16_t>(y + h / 2); }
    constexpr bool    empty()  const { return w <= 0 || h <= 0; }

    constexpr Rect inset(int16_t d) const {
        return Rect{static_cast<int16_t>(x + d), static_cast<int16_t>(y + d),
                    static_cast<int16_t>(w - 2 * d), static_cast<int16_t>(h - 2 * d)};
    }
    constexpr bool contains(int16_t px, int16_t py) const {
        return px >= x && py >= y && px < right() && py < bottom();
    }
    constexpr bool intersects(const Rect& o) const {
        return !empty() && !o.empty() && x < o.right() && o.x < right() &&
               y < o.bottom() && o.y < bottom();
    }
    // The overlap, or an empty rectangle.
    constexpr Rect clipped_to(const Rect& o) const {
        const int16_t l = x > o.x ? x : o.x;
        const int16_t t = y > o.y ? y : o.y;
        const int16_t r = right() < o.right() ? right() : o.right();
        const int16_t b = bottom() < o.bottom() ? bottom() : o.bottom();
        if (r <= l || b <= t) return Rect{};
        return Rect{l, t, static_cast<int16_t>(r - l), static_cast<int16_t>(b - t)};
    }
};

enum class Align : uint8_t { Left = 0, Center, Right };

// The panel, in the orientation the dash is mounted: **landscape**, 480
// wide by 320 tall. The ILI9488 is a 320x480 part; the driver rotates it.
// Every screen layout is written against these two numbers.
inline constexpr int16_t kScreenW = 480;
inline constexpr int16_t kScreenH = 320;

}  // namespace ui
