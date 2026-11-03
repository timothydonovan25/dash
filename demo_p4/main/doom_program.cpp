// See doom_program.hpp.
#include "doom_program.hpp"
#include "p4_panel.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/geometry.hpp"

#include "esp_heap_caps.h"

namespace demo {
namespace {

// Doom's 320x200 at 2x is 640x400: the left of the 800x480 view, centred
// top to bottom. The right 160 px is the button legend, over the touch zones.
constexpr int kScale = 2;
constexpr int kPicX = 0;
constexpr int kPicY = (480 - doom::kHeight * kScale) / 2;   // 40
constexpr int kLegendX = doom::kWidth * kScale;             // 640
constexpr int kLegendW = 800 - kLegendX;                    // 160
constexpr int kLegendH = 480;
constexpr int kBandH = kLegendH / 5;                        // touch_buttons' bands

// The legend down the right edge, one band per button.
void draw_legend(P4Panel& panel) {
    auto* buf = static_cast<ui::Color*>(heap_caps_malloc(
        static_cast<size_t>(kLegendW) * kLegendH * sizeof(ui::Color),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) return;
    ui::FrameBufferCanvas fb(buf, kLegendW, kLegendH);
    ui::Canvas& c = fb;   // the base declares draw_text's defaults
    c.fill(ui::color::kBlack);
    struct Band { const char* key; const char* top; const char* sub; };
    constexpr Band kBands[5] = {
        {"A", "FWD",   "A+B MENU"},
        {"B", "BACK",  "A+B MENU"},
        {"C", "LEFT",  "C+D USE"},
        {"D", "RIGHT", "C+D USE"},
        {"E", "FIRE",  "OK / YES"},
    };
    for (int i = 0; i < 5; ++i) {
        const int16_t y = static_cast<int16_t>(i * kBandH);
        if (i) c.fill_rect(ui::Rect{0, y, kLegendW, 1}, ui::color::kDim);
        c.draw_text(12, static_cast<int16_t>(y + 14), kBands[i].key, ui::color::kAccent, 3);
        c.draw_text(44, static_cast<int16_t>(y + 14), kBands[i].top, ui::color::kText, 3);
        c.draw_text(12, static_cast<int16_t>(y + 58), kBands[i].sub, ui::color::kDim, 2);
    }
    panel.blit_view(kLegendX, 0, kLegendW, kLegendH, buf, 1);
    heap_caps_free(buf);
}

}  // namespace

void DoomBoard::begin() {
    // Black everywhere Doom does not draw, then the legend.
    static uint16_t black[800] = {};
    for (int y = 0; y < 480; ++y) panel_.blit_view(0, y, 800, 1, black, 1);
    draw_legend(panel_);
}

void DoomBoard::frame(const uint16_t* rgb565) {
    panel_.blit_view(kPicX, kPicY, doom::kWidth, doom::kHeight, rgb565, kScale);
}

}  // namespace demo
