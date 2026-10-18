// See p4_panel.hpp — this whole file targets one specific real board
// (Guition JC4880P443C_I_W) and follows the bring-up recipe confirmed
// working for it in elik745i/ESP32-2432S024C-Remote
// (src/jc4880p443_bringup_main.cpp, boards/jc4880p443c_i_w.json) rather
// than the generic esp_lcd_st7701 vendor driver this file used at first.
// That component's own init path (`esp_lcd_new_panel_st7701` +
// `esp_lcd_panel_init`) hung indefinitely on this hardware even after
// fixing the DSI PHY power sequencing and matching every parameter to a
// same-chip reference — the missing piece turned out to be that this
// panel has a real hardware reset line (GPIO5) that has to be pulsed
// before it will answer anything, which the generic driver never learned
// about because we had it configured as GPIO_NUM_NC. See
// docs/bringup-p4-demo.md.
//
// The DPI panel gives us a real framebuffer in memory that the DSI
// peripheral scans out continuously; there is no bus transaction to push a
// blit over, the way the real board's SPI ILI9488 needs. blit() therefore
// just writes into that buffer, with a 90-degree rotation (the module is
// mounted landscape but the panel is natively portrait, 480 wide x 800
// tall) and a fixed offset that centers the UI's 480x320 canvas in the
// middle of the resulting 800x480 view.
#include "p4_panel.hpp"
#include "p4_pins.hpp"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui/geometry.hpp"
#include "ui/framebuffer_canvas.hpp"

#include <algorithm>

namespace demo {
namespace {

constexpr const char* kTag = "p4_panel";

// If the picture comes up sideways or mirrored on first boot, this is the
// first thing to flip — see docs/bringup-p4-demo.md.
constexpr bool kRotateCw = true;

constexpr int kNativeRows = p4pins::kPanelNativeH;   // 800
constexpr int kNativeCols = p4pins::kPanelNativeW;   // 480

// The UI's 480x320 canvas, scaled 1.5x to 720x480 and centered in the
// 800(w) x 480(h) view you get once the module is physically rotated 90
// degrees into the landscape case. 1.5x fills the view's height exactly
// (320*1.5=480) with only side margins left, and — unlike stretching
// independently per axis to fill all 800x480 — doesn't distort circles
// into ovals or squash the bitmap font, since it scales both axes by the
// same factor. See the plan behind this folder for the reasoning.
constexpr int kViewW = kNativeRows;   // 800
constexpr int kViewH = kNativeCols;   // 480
constexpr int kScaleNum = 3;
constexpr int kScaleDen = 2;   // 3/2 = 1.5x
constexpr int kMarginX = (kViewW - ui::kScreenW * kScaleNum / kScaleDen) / 2;   // 40
constexpr int kMarginY = (kViewH - ui::kScreenH * kScaleNum / kScaleDen) / 2;   // 0

// Button zones: five bands stacked top-to-bottom (A-E, matching sim/
// dash_sim.py's BUTTONS list and column layout), touch-sensitive across a
// 200px-wide strip on the right (see touch_buttons.cpp's kZoneWidth, which
// has to agree with this). Only the part of that strip actually outside
// the picture — view x in [kViewW-kMarginX, kViewW), the same right margin
// blit() never writes to — gets divider lines drawn into it, since
// anything further left is inside the UI's own drawing and would be
// painted over on the next frame anyway.
constexpr int kButtonCount = 5;
constexpr int kBandH = kViewH / kButtonCount;   // 96

// Maps a view-space pixel (post-rotation coordinate frame, same one
// blit()'s callers think in) to an index into the native rotated
// framebuffer, or -1 if it falls outside the panel. Shared by blit() and
// the one-time margin decoration below so both transforms can't drift
// apart.
int native_index(int view_row, int view_col) {
    int nr, nc;
    if (kRotateCw) {
        nr = (kNativeRows - 1) - view_col;
        nc = view_row;
    } else {
        nr = view_col;
        nc = (kNativeCols - 1) - view_row;
    }
    if (nr < 0 || nr >= kNativeRows || nc < 0 || nc >= kNativeCols) return -1;
    return nr * kNativeCols + nc;
}

constexpr ledc_timer_t   kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChan  = LEDC_CHANNEL_0;
constexpr ledc_mode_t    kBacklightMode  = LEDC_LOW_SPEED_MODE;
constexpr uint32_t       kBacklightBits  = 10;   // 0-1023

// VDD_MIPI_DPHY needs 2.5V from one of the chip's internal LDO channels
// before the DSI peripheral does anything at all — fixed silicon wiring on
// every ESP32-P4, confirmed by both Espressif's own mipi_dsi example and
// the jc4880p443c_i_w bring-up sketch (same channel and voltage in both).
constexpr int kDsiPhyLdoChan = 3;
constexpr int kDsiPhyLdoMv   = 2500;

// The panel's own init command table — copied from
// src/jc4880p443_bringup_main.cpp's PANEL_INIT_CMDS, which is specific to
// this physical panel's gamma/timing registers, not a generic ST7701
// default. Sent as raw DBI parameter writes rather than through a vendor
// panel driver's own init() — see the file comment for why.
struct InitCmd {
    uint8_t cmd;
    const uint8_t* data;
    size_t len;
    uint16_t delay_ms;
};

const uint8_t kFF13[] = {0x77, 0x01, 0x00, 0x00, 0x13};
const uint8_t kEF08[] = {0x08};
const uint8_t kFF10[] = {0x77, 0x01, 0x00, 0x00, 0x10};
const uint8_t kC0[]   = {0x63, 0x00};
const uint8_t kC1[]   = {0x0D, 0x02};
const uint8_t kC2[]   = {0x10, 0x08};
const uint8_t kCC[]   = {0x10};
const uint8_t kB0Gamma[] = {0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71};
const uint8_t kB1Gamma[] = {0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D};
const uint8_t kFF11[] = {0x77, 0x01, 0x00, 0x00, 0x11};
const uint8_t kB0[]   = {0x5D};
const uint8_t kB1[]   = {0x58};
const uint8_t kB2[]   = {0x87};
const uint8_t kB3[]   = {0x80};
const uint8_t kB5[]   = {0x4E};
const uint8_t kB7[]   = {0x85};
const uint8_t kB8[]   = {0x21};
const uint8_t kB9[]   = {0x10, 0x1F};
const uint8_t kBB[]   = {0x03};
const uint8_t kBC[]   = {0x00};
const uint8_t kC178[] = {0x78};
const uint8_t kC278[] = {0x78};
const uint8_t kD0[]   = {0x88};
const uint8_t kE0[]   = {0x00, 0x3A, 0x02};
const uint8_t kE1[]   = {0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40};
const uint8_t kE2[]   = {0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00};
const uint8_t kE3[]   = {0x00, 0x00, 0x33, 0x33};
const uint8_t kE4[]   = {0x44, 0x44};
const uint8_t kE5[]   = {0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0};
const uint8_t kE6[]   = {0x00, 0x00, 0x33, 0x33};
const uint8_t kE7[]   = {0x44, 0x44};
const uint8_t kE8[]   = {0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0};
const uint8_t kEB[]   = {0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00};
const uint8_t kEC[]   = {0x08, 0x01};
const uint8_t kED[]   = {0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B};
const uint8_t kEF[]   = {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54};
const uint8_t kFF00[] = {0x77, 0x01, 0x00, 0x00, 0x00};
const uint8_t kNop[]  = {0x00};

const InitCmd kInitCmds[] = {
    {0xFF, kFF13, sizeof(kFF13), 0},
    {0xEF, kEF08, sizeof(kEF08), 0},
    {0xFF, kFF10, sizeof(kFF10), 0},
    {0xC0, kC0, sizeof(kC0), 0},
    {0xC1, kC1, sizeof(kC1), 0},
    {0xC2, kC2, sizeof(kC2), 0},
    {0xCC, kCC, sizeof(kCC), 0},
    {0xB0, kB0Gamma, sizeof(kB0Gamma), 0},
    {0xB1, kB1Gamma, sizeof(kB1Gamma), 0},
    {0xFF, kFF11, sizeof(kFF11), 0},
    {0xB0, kB0, sizeof(kB0), 0},
    {0xB1, kB1, sizeof(kB1), 0},
    {0xB2, kB2, sizeof(kB2), 0},
    {0xB3, kB3, sizeof(kB3), 0},
    {0xB5, kB5, sizeof(kB5), 0},
    {0xB7, kB7, sizeof(kB7), 0},
    {0xB8, kB8, sizeof(kB8), 0},
    {0xB9, kB9, sizeof(kB9), 0},
    {0xBB, kBB, sizeof(kBB), 0},
    {0xBC, kBC, sizeof(kBC), 0},
    {0xC1, kC178, sizeof(kC178), 0},
    {0xC2, kC278, sizeof(kC278), 0},
    {0xD0, kD0, sizeof(kD0), 0},
    {0xE0, kE0, sizeof(kE0), 0},
    {0xE1, kE1, sizeof(kE1), 0},
    {0xE2, kE2, sizeof(kE2), 0},
    {0xE3, kE3, sizeof(kE3), 0},
    {0xE4, kE4, sizeof(kE4), 0},
    {0xE5, kE5, sizeof(kE5), 0},
    {0xE6, kE6, sizeof(kE6), 0},
    {0xE7, kE7, sizeof(kE7), 0},
    {0xE8, kE8, sizeof(kE8), 0},
    {0xEB, kEB, sizeof(kEB), 0},
    {0xEC, kEC, sizeof(kEC), 0},
    {0xED, kED, sizeof(kED), 0},
    {0xEF, kEF, sizeof(kEF), 0},
    {0xFF, kFF00, sizeof(kFF00), 0},
    {0x11, kNop, sizeof(kNop), 120},   // sleep out
    {0x29, kNop, sizeof(kNop), 20},    // display on
};

void pulse_panel_reset() {
    const auto rst = static_cast<gpio_num_t>(p4pins::kPanelReset);
    gpio_reset_pin(rst);
    gpio_set_direction(rst, GPIO_MODE_OUTPUT);
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

}  // namespace

hal::Status P4Panel::init() {
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = kDsiPhyLdoChan;
    ldo_cfg.voltage_mv = kDsiPhyLdoMv;
    esp_ldo_channel_handle_t ldo = nullptr;
    if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) {
        ESP_LOGE(kTag, "esp_ldo_acquire_channel (DSI PHY power) failed");
        return hal::Status::IoError;
    }

    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = 500;   // matches jc4880p443c_i_w bring-up

    esp_lcd_dsi_bus_handle_t bus = nullptr;
    if (esp_lcd_new_dsi_bus(&bus_config, &bus) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_new_dsi_bus failed");
        return hal::Status::IoError;
    }
    dsi_bus_ = bus;

    esp_lcd_dbi_io_config_t dbi_config = {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;

    esp_lcd_panel_io_handle_t io = nullptr;
    if (esp_lcd_new_panel_io_dbi(bus, &dbi_config, &io) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_new_panel_io_dbi failed");
        return hal::Status::IoError;
    }
    io_ = io;

    // Bare DPI panel, not the esp_lcd_st7701 vendor wrapper — see the file
    // comment. Timing matches jc4880p443c_i_w's bring-up sketch exactly.
    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 34;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = kNativeCols;
    dpi_config.video_timing.v_size = kNativeRows;
    dpi_config.video_timing.hsync_pulse_width = 12;
    dpi_config.video_timing.hsync_back_porch  = 42;
    dpi_config.video_timing.hsync_front_porch = 42;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_back_porch  = 8;
    dpi_config.video_timing.vsync_front_porch = 166;

    esp_lcd_panel_handle_t panel = nullptr;
    if (esp_lcd_new_panel_dpi(bus, &dpi_config, &panel) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_new_panel_dpi failed");
        return hal::Status::IoError;
    }
    panel_ = panel;

    // The reset line the generic vendor driver never knew about.
    pulse_panel_reset();

    for (const InitCmd& c : kInitCmds) {
        if (esp_lcd_panel_io_tx_param(io, c.cmd, c.data, c.len) != ESP_OK) {
            ESP_LOGE(kTag, "init cmd 0x%02X failed", c.cmd);
            return hal::Status::IoError;
        }
        if (c.delay_ms) vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
    }

    if (esp_lcd_panel_init(panel) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_panel_init failed");
        return hal::Status::IoError;
    }

    void* fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
        ESP_LOGE(kTag, "esp_lcd_dpi_panel_get_frame_buffer failed");
        return hal::Status::IoError;
    }
    fb_ = static_cast<uint16_t*>(fb);

    // The margins are never touched by blit() (only the centered UI
    // rectangle is), so whatever the DPI panel's memory happened to hold at
    // power-on would otherwise show through as noise. Black it out once,
    // then mark the four boundaries between the five touch zones (see the
    // constants above) — only across the actual margin width, since a line
    // drawn further into the picture area would just get painted over on
    // the next frame.
    std::fill(fb_, fb_ + static_cast<size_t>(kNativeRows) * kNativeCols, 0);
    for (int band = 1; band < kButtonCount; ++band) {
        const int view_row = band * kBandH;
        for (int view_col = kViewW - kMarginX; view_col < kViewW; ++view_col) {
            const int idx = native_index(view_row, view_col);
            if (idx >= 0) fb_[idx] = 0xFFFF;
        }
    }

    // A-E in each zone, small (the dash's own 5x7 font at scale 1, same
    // one ui/ draws everything else with), rendered upright into a scratch
    // buffer and copied in pixel by pixel through the same view->native
    // transform — reusing the real font instead of hand-drawing letters
    // sideways into the rotated buffer directly.
    {
        ui::Color glyph[8 * 8] = {};
        ui::FrameBufferCanvas glyph_canvas(glyph, 8, 8);
        const ui::Font& font = ui::font_5x7();
        for (int band = 0; band < kButtonCount; ++band) {
            glyph_canvas.fill(0);
            const char label[2] = {static_cast<char>('A' + band), 0};
            glyph_canvas.draw_text(0, 0, label, 0xFFFF, 1, ui::Align::Left);

            const int row0 = band * kBandH + (kBandH - font.height) / 2;
            const int col0 = (kViewW - kMarginX) + (kMarginX - font.width) / 2;
            for (int gy = 0; gy < font.height; ++gy) {
                for (int gx = 0; gx < font.width; ++gx) {
                    const ui::Color px = glyph[gy * 8 + gx];
                    if (!px) continue;
                    const int idx = native_index(row0 + gy, col0 + gx);
                    if (idx >= 0) fb_[idx] = px;
                }
            }
        }
    }

    const auto bl_gpio = static_cast<gpio_num_t>(p4pins::kPanelBacklight);
    gpio_reset_pin(bl_gpio);
    gpio_set_direction(bl_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(bl_gpio, 1);
    backlight_gpio_only_ = true;

    // LEDC PWM for actual dimming — upgrades set_backlight() to real
    // dimming if it configures; falls back to the plain on/off above if
    // not (see docs/bringup-p4-demo.md for why that fallback exists).
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = kBacklightMode;
    timer_cfg.timer_num = kBacklightTimer;
    timer_cfg.duty_resolution = static_cast<ledc_timer_bit_t>(kBacklightBits);
    timer_cfg.freq_hz = 5000;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err == ESP_OK) {
        ledc_channel_config_t chan_cfg = {};
        chan_cfg.gpio_num = p4pins::kPanelBacklight;
        chan_cfg.speed_mode = kBacklightMode;
        chan_cfg.channel = kBacklightChan;
        chan_cfg.timer_sel = kBacklightTimer;
        chan_cfg.duty = (1u << kBacklightBits) - 1;   // start at full, matching
                                                       // the plain GPIO state above
        chan_cfg.hpoint = 0;
        err = ledc_channel_config(&chan_cfg);
        if (err == ESP_OK) backlight_gpio_only_ = false;
    }
    if (backlight_gpio_only_) {
        ESP_LOGW(kTag, "LEDC backlight dimming unavailable (%s); backlight is on/off only",
                esp_err_to_name(err));
    }

    return hal::Status::Ok;
}

uint16_t P4Panel::width() const  { return ui::kScreenW; }
uint16_t P4Panel::height() const { return ui::kScreenH; }

hal::Status P4Panel::blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t* pixels) {
    if (!fb_) return hal::Status::NotPresent;

    if (w == 0 || h == 0 || !pixels) return hal::Status::Ok;

    // Nearest-neighbor 1.5x upscale: walk the *destination* pixels for this
    // rect (not the source ones), so every output pixel gets written even
    // though 1.5x means some source pixels cover a 2x2 block and others a
    // 1x1 — walking the source the other way round would leave gaps.
    //
    // Rounded up at both ends: destination d shows source floor(d / 1.5), so
    // source s owns [ceil(1.5 s), ceil(1.5 (s + 1))). Rounded down, a
    // rectangle starting at an odd x or y began one destination pixel early,
    // on its left neighbour's pixel, and read it from before its own buffer
    // -- a column of garbage at a negative index, a row of it from far past
    // the end. Widgets mostly start even, so it hid until 1.21's Shadow sent
    // small parts starting anywhere, and the start-up sweep smeared.
    const auto up = [](int v) { return (v * kScaleNum + kScaleDen - 1) / kScaleDen; };
    const int dst_x0 = up(x);
    const int dst_x1 = up(x + w);
    const int dst_y0 = up(y);
    const int dst_y1 = up(y + h);
    if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0) return hal::Status::Ok;

    // The whole destination must land on the panel; checked once at the
    // corners rather than for every pixel. A stray write into someone
    // else's PSRAM is far worse than a dropped rectangle.
    const int first = native_index(kMarginY + dst_y0, kMarginX + dst_x0);
    const int last = native_index(kMarginY + dst_y1 - 1, kMarginX + dst_x1 - 1);
    if (first < 0 || last < 0 ||
        native_index(kMarginY + dst_y1 - 1, kMarginX + dst_x0) < 0 ||
        native_index(kMarginY + dst_y0, kMarginX + dst_x1 - 1) < 0) {
        return hal::Status::Unsupported;
    }

    // Source row for every destination row, worked out once (at most 480).
    uint16_t* src_row = src_row_;
    static_assert(sizeof src_row_ / sizeof src_row_[0] >= kViewH, "src_row_ too small");
    for (int dvy = dst_y0; dvy < dst_y1; ++dvy) {
        int sy = dvy * kScaleDen / kScaleNum;
        if (sy < y) sy = y;                 // cannot happen; never read before the buffer
        if (sy > y + h - 1) sy = y + h - 1;
        src_row[dvy - dst_y0] = static_cast<uint16_t>(sy - y);
    }

    // In the framebuffer's own order. The panel is turned a quarter, so a
    // view *column* is a native row and stepping down the view is stepping
    // along it: one sequential run of stores per column instead of a 960-
    // byte stride per pixel, with the bounds and clamps above, not in here.
    const int step = kRotateCw ? 1 : -1;
    for (int dvx = dst_x0; dvx < dst_x1; ++dvx) {
        int sx = dvx * kScaleDen / kScaleNum;
        if (sx < x) sx = x;
        if (sx > x + w - 1) sx = x + w - 1;
        const uint16_t* col = pixels + (sx - x);
        uint16_t* out = fb_ + native_index(kMarginY + dst_y0, kMarginX + dvx);
        for (int i = 0, n = dst_y1 - dst_y0; i < n; ++i, out += step) {
            *out = col[static_cast<size_t>(src_row[i]) * w];
        }
    }

    // The framebuffer is PSRAM behind the CPU's cache, and the DSI DMA reads
    // PSRAM, not the cache: without a write-back, pixels reach the glass
    // only when the cache happens to evict them. The IDF's own draw_bitmap
    // does this; writing the buffer directly means doing it here. The span
    // is the native rows this touched, start to end.
    int lo = first, hi = last;
    for (int idx : {native_index(kMarginY + dst_y1 - 1, kMarginX + dst_x0),
                    native_index(kMarginY + dst_y0, kMarginX + dst_x1 - 1)}) {
        if (idx < lo) lo = idx;
        if (idx > hi) hi = idx;
    }
    if (hi < lo) std::swap(lo, hi);
    esp_cache_msync(fb_ + lo, static_cast<size_t>(hi - lo + 1) * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    return hal::Status::Ok;
}

hal::Status P4Panel::blit_view(int x, int y, int w, int h, const uint16_t* pixels,
                               int scale) {
    if (!fb_) return hal::Status::NotPresent;
    if (!pixels || w <= 0 || h <= 0 || scale < 1) return hal::Status::Ok;
    const int dw = w * scale;
    const int dh = h * scale;
    if (x < 0 || y < 0 || x + dw > kViewW || y + dh > kViewH) return hal::Status::Unsupported;

    // As blit(): a view column is a native row, so each destination column
    // is one run of stores along it, here with every source pixel repeated
    // `scale` times down the run and every source column `scale` times.
    const int step = kRotateCw ? 1 : -1;
    for (int sx = 0; sx < w; ++sx) {
        const uint16_t* col = pixels + sx;
        for (int k = 0; k < scale; ++k) {
            uint16_t* out = fb_ + native_index(y, x + sx * scale + k);
            for (int sy = 0; sy < h; ++sy) {
                const uint16_t px = col[static_cast<size_t>(sy) * w];
                for (int r = 0; r < scale; ++r, out += step) *out = px;
            }
        }
    }

    // Write the touched native rows back past the cache, as blit() does.
    int lo = native_index(y, x), hi = lo;
    for (int idx : {native_index(y + dh - 1, x), native_index(y, x + dw - 1),
                    native_index(y + dh - 1, x + dw - 1)}) {
        if (idx < lo) lo = idx;
        if (idx > hi) hi = idx;
    }
    esp_cache_msync(fb_ + lo, static_cast<size_t>(hi - lo + 1) * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    return hal::Status::Ok;
}

hal::Status P4Panel::set_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    if (backlight_gpio_only_) {
        gpio_set_level(static_cast<gpio_num_t>(p4pins::kPanelBacklight), percent > 0 ? 1 : 0);
        return hal::Status::Ok;
    }
    const uint32_t max_duty = (1u << kBacklightBits) - 1;
    const uint32_t duty = (max_duty * percent) / 100;
    ledc_set_duty(kBacklightMode, kBacklightChan, duty);
    ledc_update_duty(kBacklightMode, kBacklightChan);
    return hal::Status::Ok;
}

}  // namespace demo
