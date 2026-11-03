#include "doom_board.hpp"
#include "bsp/pins.hpp"
#include "ui/geometry.hpp"

#include "esp_heap_caps.h"
#include <algorithm>
#include <cstring>

namespace wiring {
namespace {

// 320x200 at 1.5x is 480x300: the panel's width, 10 px of black above and below.
constexpr int kW = doom::kWidth * 3 / 2;
constexpr int kH = doom::kHeight * 3 / 2;
constexpr int kY0 = (ui::kScreenH - kH) / 2;
constexpr int kBandLines = bsp::kTftBandLines;
static_assert(kW == ui::kScreenW, "the picture is the panel's width");

}  // namespace

void DoomBoard::begin() {
    if (!band_) {
        band_ = static_cast<uint16_t*>(heap_caps_malloc(
            static_cast<size_t>(kW) * kBandLines * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!band_ || !d_.panel) return;
    std::memset(band_, 0, static_cast<size_t>(kW) * kBandLines * sizeof(uint16_t));
    for (int y = 0; y < ui::kScreenH; y += kBandLines) {
        const int h = std::min(kBandLines, ui::kScreenH - y);
        d_.panel->blit(0, static_cast<uint16_t>(y), kW, static_cast<uint16_t>(h), band_);
    }
}

void DoomBoard::frame(const uint16_t* in) {
    if (!band_ || !d_.panel) return;
    for (int y = 0; y < kH; y += kBandLines) {
        const int h = std::min(kBandLines, kH - y);
        for (int r = 0; r < h; ++r) {
            // Nearest: every two source pixels (and rows) become three.
            const uint16_t* src = in + static_cast<size_t>((y + r) * 2 / 3) * doom::kWidth;
            uint16_t* out = band_ + static_cast<size_t>(r) * kW;
            for (int x = 0; x < doom::kWidth; x += 2) {
                *out++ = src[x];
                *out++ = src[x];
                *out++ = src[x + 1];
            }
        }
        // A failed write is one bad picture; the next one goes out whole anyway.
        d_.panel->blit(0, static_cast<uint16_t>(kY0 + y), kW, static_cast<uint16_t>(h), band_);
    }
}

}  // namespace wiring
