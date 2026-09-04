// See p4_panel.hpp — this whole file is unverified against real hardware.
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
#include "esp_lcd_st7701.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include "ui/geometry.hpp"

namespace demo {
namespace {

constexpr const char* kTag = "p4_panel";

// If the picture comes up sideways or mirrored on first boot, this is the
// first thing to flip — see docs/bringup-p4-demo.md.
constexpr bool kRotateCw = true;

constexpr int kNativeRows = p4pins::kPanelNativeH;   // 800
constexpr int kNativeCols = p4pins::kPanelNativeW;   // 480

// The UI's 480x320 canvas, centered in the 800(w) x 480(h) view you get
// once the module is physically rotated 90 degrees into the landscape case.
constexpr int kViewW = kNativeRows;   // 800
constexpr int kViewH = kNativeCols;   // 480
constexpr int kMarginX = (kViewW - ui::kScreenW) / 2;   // 160
constexpr int kMarginY = (kViewH - ui::kScreenH) / 2;   // 80

constexpr ledc_timer_t   kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChan  = LEDC_CHANNEL_0;
constexpr ledc_mode_t    kBacklightMode  = LEDC_LOW_SPEED_MODE;
constexpr uint32_t       kBacklightBits  = 10;   // 0-1023

// VDD_MIPI_DPHY needs 2.5V from one of the chip's internal LDO channels
// before the DSI peripheral does anything at all — this isn't a per-board
// pin, it's fixed silicon wiring on every ESP32-P4, and Espressif's own
// examples and BSPs (esp-idf's peripherals/lcd/mipi_dsi example; the
// esp32_p4_function_ev_board BSP a matching board's published firmware
// uses) all acquire the same channel. Missing this is exactly why every
// DSI transaction hung forever regardless of lane count or bit rate: the
// bus "existed" in software but had no power, so nothing ever happened on
// the wire for the status registers to react to.
constexpr int      kDsiPhyLdoChan = 3;
constexpr int      kDsiPhyLdoMv   = 2500;

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
    // Confirmed against the published firmware for this exact board model
    // (Guition JC4880P443C) — not a guess like the earlier 1000/500/480
    // attempts were.
    bus_config.lane_bit_rate_mbps = 750;

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

    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 25;   // typical for a 480x800 panel
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = kNativeCols;
    dpi_config.video_timing.v_size = kNativeRows;
    // Placeholder porch/sync values — every one of these needs confirming
    // against whatever the panel's real datasheet timing turns out to be.
    dpi_config.video_timing.hsync_pulse_width = 8;
    dpi_config.video_timing.hsync_back_porch  = 20;
    dpi_config.video_timing.hsync_front_porch = 20;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_back_porch  = 12;
    dpi_config.video_timing.vsync_front_porch = 12;

    st7701_vendor_config_t vendor_config = {};
    vendor_config.mipi_config.dsi_bus = bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.flags.use_mipi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    // GPIO_NUM_NC: the published firmware for this exact board (Guition
    // JC4880P443C) doesn't drive a reset pin for this panel either — it
    // relies on the DSI init command sequence rather than a hardware
    // reset line. kPanelReset in p4_pins.hpp is unused now; kept only as
    // the historical placeholder in case that turns out wrong.
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    esp_lcd_panel_handle_t panel = nullptr;
    if (esp_lcd_new_panel_st7701(io, &panel_config, &panel) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lcd_new_panel_st7701 failed");
        return hal::Status::IoError;
    }
    panel_ = panel;

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    void* fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
        ESP_LOGE(kTag, "esp_lcd_dpi_panel_get_frame_buffer failed");
        return hal::Status::IoError;
    }
    fb_ = static_cast<uint16_t*>(fb);

    // GPIO26 is confirmed correct (found by demo_p4/main/backlight_scan.cpp
    // driving it as a plain digital output — see docs/bringup-p4-demo.md).
    // Force it on immediately and unconditionally so the panel has a
    // backlight even if the LEDC dimmer below fails to configure — a
    // guaranteed-on screen beats a dim-but-silently-broken one for a demo.
    const auto bl_gpio = static_cast<gpio_num_t>(p4pins::kPanelBacklight);
    gpio_reset_pin(bl_gpio);
    gpio_set_direction(bl_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(bl_gpio, 1);
    backlight_gpio_only_ = true;

    // LEDC PWM for actual dimming, same idea as drivers::Ili9488's — a
    // GPIO of its own rather than a bit on the shared MCP23S17. Upgrades
    // set_backlight() to real dimming if it configures; falls back to the
    // plain on/off above if not.
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

    for (uint16_t row = 0; row < h; ++row) {
        const int ux = x;
        const int uy = y + row;
        const uint16_t* src = pixels + static_cast<size_t>(row) * w;
        for (uint16_t col = 0; col < w; ++col) {
            const int view_col = kMarginX + ux + col;   // 0..799
            const int view_row = kMarginY + uy;          // 0..479
            int nr, nc;
            if (kRotateCw) {
                nr = (kNativeRows - 1) - view_col;
                nc = view_row;
            } else {
                nr = view_col;
                nc = (kNativeCols - 1) - view_row;
            }
            if (nr < 0 || nr >= kNativeRows || nc < 0 || nc >= kNativeCols) {
                continue;   // shouldn't happen with a correct margin, but a
                            // stray write into someone else's PSRAM is far
                            // worse than a dropped pixel
            }
            fb_[static_cast<size_t>(nr) * kNativeCols + nc] = src[col];
        }
    }
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
