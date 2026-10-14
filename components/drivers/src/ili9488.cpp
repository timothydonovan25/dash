#include "drivers/ili9488.hpp"
#include "drivers/pca9955b.hpp"
#include "bsp/pins.hpp"

#include "esp_heap_caps.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include <cstring>

namespace drivers {
namespace {

// The bus's DMA burst, from bsp/buses.cpp. Every color transfer's address
// and length must be a multiple of it.
constexpr size_t kDmaAlign = 32;
constexpr size_t kStagingBytes = static_cast<size_t>(bsp::kTftMaxBlitPx) * 2;

constexpr int kCaset    = 0x2A;
constexpr int kPaset    = 0x2B;
constexpr int kRamwr    = 0x2C;
constexpr int kRamwrCon = 0x3C;   // Memory Write Continue

hal::Status from_esp(esp_err_t err) {
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
    if (!bus_) return hal::Status::NotPresent;

    if (!staging_) {
        staging_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            64, kStagingBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!staging_) return hal::Status::IoError;
    }

    if (!io_) {
        esp_lcd_panel_io_i80_config_t cfg = {};
        cfg.cs_gpio_num = static_cast<gpio_num_t>(bsp::kTftCs);
        cfg.pclk_hz = bsp::kTftWrHz;
        cfg.trans_queue_depth = 4;
        cfg.lcd_cmd_bits = 8;
        cfg.lcd_param_bits = 8;
        cfg.dc_levels.dc_idle_level = 0;
        cfg.dc_levels.dc_cmd_level = 0;
        cfg.dc_levels.dc_dummy_level = 0;
        cfg.dc_levels.dc_data_level = 1;
        // Colour data only; command parameters go out in the order given.
        cfg.flags.swap_color_bytes = 1;

        esp_lcd_panel_io_handle_t io = nullptr;
        const esp_err_t err = esp_lcd_new_panel_io_i80(
            static_cast<esp_lcd_i80_bus_handle_t>(bus_), &cfg, &io);
        if (err != ESP_OK) return from_esp(err);
        io_ = io;
    }

    if (!panel_) {
        esp_lcd_panel_dev_config_t panel_cfg = {};
        // A real GPIO now, not a pin on an expander that has to come up
        // first. R3 still holds the line high if this pin is not driven.
        panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(bsp::kTftRst);
        panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_cfg.bits_per_pixel = 16;          // 8080 mode takes RGB565

        esp_lcd_panel_handle_t panel = nullptr;
        // No conversion buffer: the 16-bit path never converts.
        const esp_err_t err = esp_lcd_new_panel_ili9488(
            static_cast<esp_lcd_panel_io_handle_t>(io_), &panel_cfg, 0, &panel);
        if (err != ESP_OK) return from_esp(err);
        panel_ = panel;
    }

    auto* panel = static_cast<esp_lcd_panel_handle_t>(panel_);
    esp_err_t err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) return from_esp(err);
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) return from_esp(err);
    // Landscape. swap_xy transposes, and one mirror puts the origin at the
    // top left rather than giving a mirror image — which way round depends
    // on how the panel is fitted in the case.
    //
    // BRINGUP: if the display comes up mirrored or upside down when the
    // board arrives, these two lines are the only thing to change.
    // panel_test's corners screen shows which.
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, false);
    return from_esp(esp_lcd_panel_disp_on_off(panel, true));
}

hal::Status Ili9488::blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t* pixels) {
    if (!io_ || !staging_ || !pixels) return hal::Status::NotPresent;
    if (w == 0 || h == 0) return hal::Status::Ok;
    const size_t px = static_cast<size_t>(w) * h;
    if (px > static_cast<size_t>(bsp::kTftMaxBlitPx)) return hal::Status::Unsupported;

    auto* io = static_cast<esp_lcd_panel_io_handle_t>(io_);

    // The window first. A parameter write waits for every colour transfer
    // still in flight, so once these return the previous blit's DMA has
    // finished reading the staging buffer and it is safe to refill.
    const uint16_t x1 = static_cast<uint16_t>(x + w - 1);
    const uint16_t y1 = static_cast<uint16_t>(y + h - 1);
    const uint8_t caset[4] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                              static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)};
    const uint8_t paset[4] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                              static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)};
    esp_err_t err = esp_lcd_panel_io_tx_param(io, kCaset, caset, sizeof caset);
    if (err != ESP_OK) return from_esp(err);
    err = esp_lcd_panel_io_tx_param(io, kPaset, paset, sizeof paset);
    if (err != ESP_OK) return from_esp(err);

    const size_t bytes = px * 2;
    const auto* src = reinterpret_cast<const uint8_t*>(pixels);

    // The odd bytes go first, as command parameters: fewer than sixteen
    // pixels, which bypass the DMA's 32-byte rule but also the I80 byte
    // swap, so they are swapped here. First and not last, because a
    // parameter write waits for every transfer in flight: sent after the
    // bulk, it held this call until the whole DMA had finished, which was
    // most widgets (57% of strips are not a multiple of 16 px) and threw
    // away the overlap of drawing the next widget with this one's wire time.
    // Here nothing is in flight yet -- the window writes above waited.
    //
    // BRINGUP: this relies on the panel continuing a 0x2C write with 0x3C,
    // as the old tail did the other way round. A strip edge with the wrong
    // colours in its first few pixels means it does not.
    const size_t head = bytes & (kDmaAlign - 1);
    if (head) {
        uint8_t t[kDmaAlign];
        for (size_t i = 0; i < head; i += 2) {
            t[i]     = src[i + 1];
            t[i + 1] = src[i];
        }
        err = esp_lcd_panel_io_tx_param(io, kRamwr, t, head);
        if (err != ESP_OK) return from_esp(err);
    }

    // The aligned rest by DMA, continuing the write the head started. It is
    // queued, not waited for: the next blit's window write is what waits,
    // so drawing the next widget overlaps this transfer.
    const size_t bulk = bytes - head;
    if (bulk) {
        std::memcpy(staging_, src + head, bulk);
        err = esp_lcd_panel_io_tx_color(io, head ? kRamwrCon : kRamwr, staging_, bulk);
        if (err != ESP_OK) return from_esp(err);
    }
    return hal::Status::Ok;
}

hal::Status Ili9488::set_backlight(uint8_t percent) {
    if (!backlight_) return hal::Status::NotPresent;
    return backlight_->set_backlight(percent);
}

}  // namespace drivers
