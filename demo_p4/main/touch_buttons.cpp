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
// A 200px-wide strip along the right edge, right-justified — wide enough
// to actually hit reliably (the picture's own 40px margin was too thin to
// find by feel). This overlaps the picture itself (which only leaves a
// 40px margin on that side), so only the outer 40px of it gets divider
// lines drawn — see p4_panel.cpp's kMarginX and the comment where it draws
// them — but the touch zone extends further left than the visible lines.
constexpr int kZoneWidth = 200;
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

    // The GT911 has two possible I2C addresses, chosen by the level on its
    // INT pin at the moment RST releases (its datasheet's address-select
    // procedure). ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() defaults to the
    // primary address (0x5D); that address NACKed every transaction on
    // this board, so this tries the backup address (0x14) instead.
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;

    esp_lcd_panel_io_handle_t io = nullptr;
    if (esp_lcd_new_panel_io_i2c(g_i2c_bus, &io_config, &io) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_new_panel_io_i2c failed");
        return hal::Status::IoError;
    }
    io_ = io;

    // Without this, esp_lcd_touch_gt911.c logs "I2C address initialization
    // procedure skipped - using default GT9xx setup" (seen verbatim in our
    // boot log) and never actually drives the INT/RST address-select pulse
    // at all — it just leaves the chip however it powered up. Passing this
    // struct via driver_data is what turns that procedure on and tells it
    // which address to latch.
    static esp_lcd_touch_io_gt911_config_t gt911_addr_cfg = {};
    gt911_addr_cfg.dev_addr = io_config.dev_addr;

    esp_lcd_touch_config_t tp_cfg = {};
    // x_max/y_max look like they should be the final (post-swap) view
    // size, but esp_lcd_touch.c applies mirror_x/mirror_y to the RAW axes
    // (x[i] = x_max - x[i]) *before* swap_xy exchanges x and y — so these
    // have to be the panel's native raw resolution, not the view size.
    // Setting them to the view size (800x480) had mirror_y computing
    // 480 - raw_y against a raw y that natively goes up to 800, underflowing
    // to ~65500 whenever a tap landed above raw_y=480 — which then landed in
    // the *x* output after the swap, confirmed by logging raw coordinates.
    tp_cfg.x_max = p4pins::kPanelNativeW;   // 480, native raw x
    tp_cfg.y_max = p4pins::kPanelNativeH;   // 800, native raw y
    tp_cfg.rst_gpio_num = static_cast<gpio_num_t>(p4pins::kTouchRst);
    tp_cfg.int_gpio_num = static_cast<gpio_num_t>(p4pins::kTouchInt);
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.driver_data = &gt911_addr_cfg;
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
        // Off the screen is a bad report, not a press. mirror_y is
        // y_max - raw_y on the raw axis, so a glitch point at raw (0, 0)
        // comes out as x = 800 -- inside the strip -- with y = 0, which is
        // band A; and a raw y past 800 underflows to a huge x that also
        // passes the strip test. Either one used to fire A while the finger
        // was on B..E. Clamping y into the last band hid the same thing at
        // the bottom edge.
        if (xs[i] >= kViewW || ys[i] >= kViewH) {
            ESP_LOGW(kTag, "ignored off-screen point %u,%u", xs[i], ys[i]);
            continue;
        }
        if (xs[i] < kViewW - kZoneWidth) continue;   // outside the button strip
        const int band = ys[i] / kBandH;              // 0..4, ys < kViewH
        mask_out |= static_cast<uint8_t>(1u << band);
    }
    return hal::Status::Ok;
}

}  // namespace demo
