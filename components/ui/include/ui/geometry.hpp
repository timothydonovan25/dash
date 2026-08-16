// Rectangles and colours. No hardware, no allocation.
#pragma once
#include <cstdint>

namespace ui {

// RGB565, because that is what the UI thinks in everywhere. The panel driver
// converts to the ILI9488's 18-bit format on its way out (§7.6).
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
};

enum class Align : uint8_t { Left = 0, Center, Right };

// The panel, in the orientation the dash is mounted: portrait, 320 wide by
// 480 tall. Every screen layout below is written against these two numbers.
inline constexpr int16_t kScreenW = 320;
inline constexpr int16_t kScreenH = 480;

}  // namespace ui
