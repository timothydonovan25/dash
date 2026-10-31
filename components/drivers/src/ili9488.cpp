#include "drivers/ili9488.hpp"
#include "drivers/pca9955b.hpp"
#include "bsp/buses.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>
#include <initializer_list>

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
constexpr int kTeon     = 0x35;   // Tearing Effect Line On, the same on both chips

// How long the probe listens for TE. The first pulse comes within one frame
// of TEON, about 17 ms at 60 Hz, so this is three.
constexpr uint32_t kTeWaitMs = 50;

SemaphoreHandle_t s_te = nullptr;

// The UEED035HV's ST7365, as VIEWE configure it: byte for byte the table in
// their UEDX32480035E-WB-A board file (ESP32_Display_Panel) and in Draftling
// (github.com/clackups/draftling PR 81), whose author ran it on this very
// panel over SPI. MADCTL is left out: swap_xy() and mirror() send it whole.
struct InitCmd {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[14];
    uint16_t wait_ms;
};
constexpr InitCmd kSt7365Init[] = {
    {0x11, 0, {}, 120},                     // sleep out
    {0xF0, 1, {0xC3}, 0},                   // command set unlock, 1 of 2
    {0xF0, 1, {0x96}, 0},                   // command set unlock, 2 of 2
    {0x3A, 1, {0x55}, 0},                   // RGB565
    {0xB4, 1, {0x01}, 0},                   // inversion control
    {0xB7, 1, {0xC6}, 0},                   // entry mode
    {0xC0, 2, {0x80, 0x04}, 0},             // power control 1
    {0xC1, 1, {0x13}, 0},                   // power control 2
    {0xC5, 1, {0xA7}, 0},                   // VCOM, as VIEWE send it twice:
    {0xC5, 1, {0x16}, 0},                   //   the second one stands
    {0xE8, 8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 0},
    {0xE0, 14, {0xF0, 0x19, 0x20, 0x10, 0x11, 0x0A, 0x46, 0x44, 0x57, 0x09,
                0x1A, 0x1B, 0x2A, 0x2D}, 0},           // positive gamma
    {0xE1, 14, {0xF0, 0x12, 0x1A, 0x0A, 0x0C, 0x18, 0x45, 0x44, 0x56, 0x3F,
                0x15, 0x11, 0x24, 0x26}, 0},           // negative gamma
    {0xF0, 1, {0x3C}, 0},                   // command set lock, 1 of 2
    {0xF0, 1, {0x69}, 0},                   // command set lock, 2 of 2
    {0x21, 0, {}, 0},                       // inversion on, or colours invert
    {0x29, 0, {}, 50},                      // display on
};

esp_err_t init_st7365(esp_lcd_panel_io_handle_t io) {
    for (const InitCmd& c : kSt7365Init) {
        const esp_err_t err = esp_lcd_panel_io_tx_param(io, c.cmd, c.len ? c.data : nullptr, c.len);
        if (err != ESP_OK) return err;
        if (c.wait_ms) vTaskDelay(pdMS_TO_TICKS(c.wait_ms));
    }
    return ESP_OK;
}

void IRAM_ATTR on_te(void*) {
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_te, &woke);
    if (woke) portYIELD_FROM_ISR();
}

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

    // A re-init after a fault keeps the wiring the probe found.
    if (probed_) return start();
    probed_ = true;

    const PanelModel guess = model_;
    const PanelModel other = guess == PanelModel::Ht035 ? PanelModel::Ueed035
                                                         : PanelModel::Ht035;
    for (PanelModel m : {guess, other}) {
        const hal::Status s = wire_for(m);
        if (!hal::ok(s)) return s;
        if (te_answers()) {
            confirmed_ = true;
            return hal::Status::Ok;
        }
    }
    // Neither answered: no TE from this panel, or no panel. Back to the
    // guess, so a panel whose TE alone is broken still shows a picture.
    return wire_for(guess);
}

// The bus, IO and panel handles for `m`, then start(). WR and DC are a bus
// setting and the write clock an IO one, so a change of wiring rebuilds
// all three.
hal::Status Ili9488::wire_for(PanelModel m) {
    const bool swap = m == PanelModel::Ueed035;
    if (swap != swapped_) {
        if (panel_) esp_lcd_panel_del(static_cast<esp_lcd_panel_handle_t>(panel_));
        if (io_) esp_lcd_panel_io_del(static_cast<esp_lcd_panel_io_handle_t>(io_));
        panel_ = nullptr;
        io_ = nullptr;
        bsp::release_lcd_bus(bus_);
        bus_ = bsp::init_lcd_bus(swap);
        swapped_ = swap;
        if (!bus_) return hal::Status::IoError;
    }
    model_ = m;

    if (!io_) {
        esp_lcd_panel_io_i80_config_t cfg = {};
        cfg.cs_gpio_num = static_cast<gpio_num_t>(bsp::kTftCs);
        cfg.pclk_hz = m == PanelModel::Ueed035 ? bsp::kTftWrHzUeed035 : bsp::kTftWrHz;
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
        // The IPS sequence: the HT035SHV007NS is IPS, and the default one
        // (TN power, VCOM and gamma, no INVON) gave it a negative image and,
        // once inverted by hand, a fine crawling speckle on every colour at
        // any write clock (first light, 2026-10-28). It ends with INVON.
        // The UEED035HV's ST7365 never runs it: start() sends VIEWE's table
        // instead, and uses this handle only for reset, swap and mirror.
        const esp_err_t err = esp_lcd_new_panel_ili9488_ips(
            static_cast<esp_lcd_panel_io_handle_t>(io_), &panel_cfg, 0, &panel);
        if (err != ESP_OK) return from_esp(err);
        panel_ = panel;
    }
    return start();
}

hal::Status Ili9488::start() {
    auto* panel = static_cast<esp_lcd_panel_handle_t>(panel_);
    esp_err_t err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) return from_esp(err);
    err = model_ == PanelModel::Ueed035
              ? init_st7365(static_cast<esp_lcd_panel_io_handle_t>(io_))
              : esp_lcd_panel_init(panel);
    if (err != ESP_OK) return from_esp(err);
    // Landscape. swap_xy transposes, and one mirror puts the origin at the
    // top left rather than giving a mirror image — which way round depends
    // on how the panel is fitted in the case.
    //
    // The first board's HT035, as fitted, needs both mirrors (2026-10-28).
    // A panel fitted another way changes only these two lines; panel_test's
    // corners screen shows which.
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, true);
    // No esp_lcd_panel_disp_on_off(panel, true) here: init() above already
    // sent DISPON, and the call sends it again and waits another 100 ms,
    // all of it before the first frame.
    return hal::Status::Ok;
}

// TEON, then wait for the first pulse. A panel whose init went in the wrong
// way round never saw TEON, and the reset left its TE output low.
bool Ili9488::te_answers() {
    static StaticSemaphore_t buf;
    if (!s_te) s_te = xSemaphoreCreateBinaryStatic(&buf);
    xSemaphoreTake(s_te, 0);

    const uint8_t vblank_only = 0x00;
    if (esp_lcd_panel_io_tx_param(static_cast<esp_lcd_panel_io_handle_t>(io_),
                                  kTeon, &vblank_only, 1) != ESP_OK) {
        return false;
    }

    const auto pin = static_cast<gpio_num_t>(bsp::kTftTe);
    gpio_config_t te = {};
    te.pin_bit_mask = 1ULL << bsp::kTftTe;
    te.mode = GPIO_MODE_INPUT;
    te.pull_down_en = GPIO_PULLDOWN_ENABLE;   // no edges from a floating pin
    te.intr_type = GPIO_INTR_POSEDGE;
    if (gpio_config(&te) != ESP_OK) return false;
    // engine_sense has usually installed the service already.
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    if (gpio_isr_handler_add(pin, on_te, nullptr) != ESP_OK) return false;

    const bool heard = xSemaphoreTake(s_te, pdMS_TO_TICKS(kTeWaitMs)) == pdTRUE;

    gpio_isr_handler_remove(pin);
    gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
    return heard;
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
