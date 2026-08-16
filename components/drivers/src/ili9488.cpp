#include "drivers/ili9488.hpp"
#include "drivers/mcp23s17.hpp"
#include "bsp/pins.hpp"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9488.h"

namespace drivers {
namespace {

constexpr ledc_timer_t   kBlTimer   = LEDC_TIMER_0;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;
constexpr ledc_mode_t    kBlMode    = LEDC_LOW_SPEED_MODE;

hal::Status from_esp(int err) {
    if (err == ESP_OK) return hal::Status::Ok;
    if (err == ESP_ERR_TIMEOUT) return hal::Status::Timeout;
    return hal::Status::IoError;
}

}  // namespace

// The part is 320x480. The dash is mounted landscape, so the panel is
// rotated in hardware and everything above this line thinks in 480x320.
uint16_t Ili9488::width() const  { return bsp::kTftHeight; }
uint16_t Ili9488::height() const { return bsp::kTftWidth; }

hal::Status Ili9488::init() {
    // The reset line is on the expander. If that is dead we carry on anyway:
    // R3 pulls the line high, so the panel comes out of reset by itself and
    // this degrades to "the screen might not have reset cleanly" rather than
    // to a dark dash.
    if (mcp_) mcp_->pulse_tft_reset();

    // SPI3 belongs to bsp, which sizes its transfers for exactly one band.
    const int max_transfer = bsp::kTftHeight * 40 * 3;   // landscape width
    esp_err_t err = ESP_OK;

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = static_cast<gpio_num_t>(bsp::kTftDc);
    io_cfg.cs_gpio_num = static_cast<gpio_num_t>(bsp::kTftCs);
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;

    esp_lcd_panel_io_handle_t io = nullptr;
    // esp_lcd_spi_bus_handle_t is an int in IDF 6, so this is a value
    // conversion rather than a pointer cast.
    err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(SPI3_HOST), &io_cfg, &io);
    if (err != ESP_OK) return from_esp(err);
    io_ = io;

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC; // it is on the expander, not a pin
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 18;          // RGB666: this panel's only option

    esp_lcd_panel_handle_t panel = nullptr;
    err = esp_lcd_new_panel_ili9488(io, &panel_cfg, max_transfer, &panel);
    if (err != ESP_OK) return from_esp(err);
    panel_ = panel;

    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) return from_esp(err);
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) return from_esp(err);
    // Landscape. swap_xy transposes, and one mirror puts the origin at the
    // top left rather than giving a mirror image — which way round depends
    // on how the panel is fitted in the case.
    //
    // BRINGUP: if the display comes up mirrored or upside down when the
    // board arrives, these two lines are the only thing to change.
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, false);

    esp_lcd_panel_disp_on_off(panel, true);

    return hal::Status::Ok;
}

hal::Status Ili9488::blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t* pixels) {
    if (!panel_ || !pixels) return hal::Status::NotPresent;
    // esp_lcd's end coordinates are exclusive. The component does the
    // RGB565-to-RGB666 expansion on the way to the panel.
    return from_esp(esp_lcd_panel_draw_bitmap(
        static_cast<esp_lcd_panel_handle_t>(panel_), x, y, x + w, y + h,
        pixels));
}

hal::Status Ili9488::set_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;

    if (!backlight_ready_) {
        ledc_timer_config_t t = {};
        t.speed_mode = kBlMode;
        t.timer_num = kBlTimer;
        t.duty_resolution = LEDC_TIMER_8_BIT;
        t.freq_hz = 5000;               // above hearing, below any flicker
        t.clk_cfg = LEDC_AUTO_CLK;
        if (ledc_timer_config(&t) != ESP_OK) return hal::Status::IoError;

        ledc_channel_config_t c = {};
        c.speed_mode = kBlMode;
        c.channel = kBlChannel;
        c.timer_sel = kBlTimer;
        c.gpio_num = bsp::kTftBacklight;
        c.duty = 0;
        if (ledc_channel_config(&c) != ESP_OK) return hal::Status::IoError;
        backlight_ready_ = true;
    }

    const uint32_t duty = (255u * percent) / 100u;
    if (ledc_set_duty(kBlMode, kBlChannel, duty) != ESP_OK) return hal::Status::IoError;
    return from_esp(ledc_update_duty(kBlMode, kBlChannel));
}

}  // namespace drivers
