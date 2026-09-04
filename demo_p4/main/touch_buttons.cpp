// See touch_buttons.hpp. Also unverified — the touch controller is assumed
// to be a GT911, the common choice for this panel class, on its own I2C
// bus. See docs/bringup-p4-demo.md.
#include "touch_buttons.hpp"
#include "p4_pins.hpp"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

namespace demo {
namespace {

constexpr const char* kTag = "touch_buttons";

// Same view geometry p4_panel.cpp centers the UI in (800 wide, 480 tall
// once the module is physically rotated to landscape).
constexpr int kViewW = 800;
constexpr int kViewH = 480;
constexpr int kMarginX = 160;             // must match p4_panel.cpp
constexpr int kButtonCount = 5;
constexpr int kBandH = kViewH / kButtonCount;

i2c_master_bus_handle_t g_i2c_bus = nullptr;

}  // namespace

hal::Status TouchButtons::init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;   // let the driver pick a free controller
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(p4pins::kTouchSda);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(p4pins::kTouchScl);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus_cfg, &g_i2c_bus) != ESP_OK) {
        ESP_LOGE(kTag, "i2c_new_master_bus failed");
        return hal::Status::IoError;
    }

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    esp_lcd_panel_io_handle_t io = nullptr;
    if (esp_lcd_new_panel_io_i2c(g_i2c_bus, &io_config, &io) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_new_panel_io_i2c failed");
        return hal::Status::IoError;
    }
    io_ = io;

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = kViewW;
    tp_cfg.y_max = kViewH;
    tp_cfg.rst_gpio_num = static_cast<gpio_num_t>(p4pins::kTouchRst);
    tp_cfg.int_gpio_num = static_cast<gpio_num_t>(p4pins::kTouchInt);
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    // The touch chip reports in the panel's native portrait frame; these
    // flags are the first thing to try flipping if taps land on the wrong
    // zone once the picture itself is confirmed right-side up.
    tp_cfg.flags.swap_xy = true;
    tp_cfg.flags.mirror_x = false;
    tp_cfg.flags.mirror_y = true;

    esp_lcd_touch_handle_t tp = nullptr;
    if (esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &tp) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_touch_new_i2c_gt911 failed");
        return hal::Status::IoError;
    }
    tp_ = tp;
    return hal::Status::Ok;
}

hal::Status TouchButtons::read_mask(uint8_t& mask_out) {
    mask_out = 0;
    if (!tp_) return hal::Status::NotPresent;

    auto* tp = static_cast<esp_lcd_touch_handle_t>(tp_);
    if (esp_lcd_touch_read_data(tp) != ESP_OK) return hal::Status::IoError;

    uint16_t xs[5] = {};
    uint16_t ys[5] = {};
    uint16_t strength[5] = {};
    uint8_t  count = 0;
    esp_lcd_touch_get_coordinates(tp, xs, ys, strength, &count, 5);

    for (uint8_t i = 0; i < count; ++i) {
        if (xs[i] >= kMarginX) continue;   // outside the button strip
        int band = ys[i] / kBandH;
        if (band < 0) band = 0;
        if (band >= kButtonCount) band = kButtonCount - 1;
        mask_out |= static_cast<uint8_t>(1u << band);
    }
    return hal::Status::Ok;
}

}  // namespace demo
