// panel_test — ILI9488 bring-up on a bare ESP32-S3 dev board.
//
// Answers docs/history/s3-parallel-respin.md §2: does this panel run on an 8-bit
// 8080 bus, are all eight data lines good, and how long does a frame take.
// The same binary in SPI mode proves the panel and wiring are alive before
// any of that is blamed on the parallel setup.
//
// The panel never acknowledges a write, so nothing here can tell from the
// ESP side that the panel works. Every check is visual: the serial log says
// what should be on screen, and README.md says what each wrong picture
// means.
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr const char* TAG = "panel_test";

// Landscape, as the dash mounts it: the 320x480 part rotated in hardware.
constexpr int kW = 480;
constexpr int kH = 320;
// Drawn in bands, like the firmware. 40 rows of 480 px is 38.4 kB at
// 16 bpp, which fits internal DMA RAM with room for the SPI conversion
// buffer as well.
constexpr int kBandRows = 40;
constexpr int kBandPx = kW * kBandRows;

constexpr int kPclkHz = CONFIG_PANEL_TEST_PCLK_MHZ * 1000 * 1000;
constexpr int kHoldMs = 3000;          // how long each picture stays up
constexpr int kTimedFrames = 60;

#if CONFIG_PANEL_TEST_IFACE_I80
constexpr bool kI80 = true;
#else
constexpr bool kI80 = false;
#endif

#if CONFIG_PANEL_TEST_FLIP_BYTE_ORDER
constexpr bool kFlip = true;
#else
constexpr bool kFlip = false;
#endif

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kRed   = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue  = 0x001F;

gpio_num_t pin(int n) { return static_cast<gpio_num_t>(n); }

struct Panel {
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t handle = nullptr;
    SemaphoreHandle_t done = nullptr;   // given when a band's DMA finishes
    uint16_t* band = nullptr;
    // On an 8-bit bus the panel wants RGB565 high byte first, and the bus
    // sends memory order, so i80 pixels are stored byte-swapped. The SPI
    // path converts native RGB565 to RGB666 itself and must not be swapped.
    bool swap = kI80 != kFlip;

    uint16_t wire(uint16_t rgb565) const {
        return swap ? static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8))
                    : rgb565;
    }
};

bool on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                   void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<Panel*>(ctx)->done, &woken);
    return woken == pdTRUE;
}

esp_err_t make_io_i80(Panel& p) {
#if CONFIG_PANEL_TEST_IFACE_I80
    esp_lcd_i80_bus_config_t bus = {};
    bus.clk_src = LCD_CLK_SRC_DEFAULT;
    bus.dc_gpio_num = pin(CONFIG_PANEL_TEST_PIN_DC);
    bus.wr_gpio_num = pin(CONFIG_PANEL_TEST_PIN_WR);
    const int data[8] = {
        CONFIG_PANEL_TEST_PIN_D0, CONFIG_PANEL_TEST_PIN_D1,
        CONFIG_PANEL_TEST_PIN_D2, CONFIG_PANEL_TEST_PIN_D3,
        CONFIG_PANEL_TEST_PIN_D4, CONFIG_PANEL_TEST_PIN_D5,
        CONFIG_PANEL_TEST_PIN_D6, CONFIG_PANEL_TEST_PIN_D7,
    };
    for (int i = 0; i < 8; ++i) bus.data_gpio_nums[i] = pin(data[i]);
    bus.bus_width = 8;
    bus.max_transfer_bytes = kBandPx * sizeof(uint16_t);

    esp_lcd_i80_bus_handle_t bus_handle = nullptr;
    esp_err_t err = esp_lcd_new_i80_bus(&bus, &bus_handle);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_i80_config_t io = {};
    io.cs_gpio_num = pin(CONFIG_PANEL_TEST_PIN_CS);
    io.pclk_hz = kPclkHz;
    io.trans_queue_depth = 10;
    io.dc_levels.dc_idle_level = 0;
    io.dc_levels.dc_cmd_level = 0;
    io.dc_levels.dc_dummy_level = 0;
    io.dc_levels.dc_data_level = 1;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    return esp_lcd_new_panel_io_i80(bus_handle, &io, &p.io);
#else
    (void)p;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t make_io_spi(Panel& p) {
#if CONFIG_PANEL_TEST_IFACE_SPI
    spi_bus_config_t bus = {};
    bus.sclk_io_num = pin(CONFIG_PANEL_TEST_PIN_WR);
    bus.mosi_io_num = pin(CONFIG_PANEL_TEST_PIN_MOSI);
    bus.miso_io_num = pin(CONFIG_PANEL_TEST_PIN_MISO);
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = kBandPx * 3;     // RGB666 on the wire
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_spi_config_t io = {};
    io.dc_gpio_num = pin(CONFIG_PANEL_TEST_PIN_DC);
    io.cs_gpio_num = pin(CONFIG_PANEL_TEST_PIN_CS);
    io.pclk_hz = kPclkHz;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.spi_mode = 0;
    io.trans_queue_depth = 10;
    return esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(SPI3_HOST), &io, &p.io);
#else
    (void)p;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bring_up(Panel& p) {
#if CONFIG_PANEL_TEST_PIN_BL >= 0
    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << CONFIG_PANEL_TEST_PIN_BL;
    bl.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl);
    gpio_set_level(pin(CONFIG_PANEL_TEST_PIN_BL), 1);
#endif

    p.done = xSemaphoreCreateBinary();
    p.band = static_cast<uint16_t*>(heap_caps_malloc(
        kBandPx * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!p.done || !p.band) return ESP_ERR_NO_MEM;

    esp_err_t err = kI80 ? make_io_i80(p) : make_io_spi(p);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = on_color_done;
    err = esp_lcd_panel_io_register_event_callbacks(p.io, &cbs, &p);
    if (err != ESP_OK) return err;

    esp_lcd_panel_dev_config_t cfg = {};
    cfg.reset_gpio_num = pin(CONFIG_PANEL_TEST_PIN_RST);
    cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    // 16 bpp is legal on the parallel bus and sent untouched. Over SPI the
    // panel only takes 18 bpp, and the driver needs a conversion buffer
    // sized in pixels for one band.
    cfg.bits_per_pixel = kI80 ? 16 : 18;
    const size_t convert_px = kI80 ? 0 : kBandPx;

#if CONFIG_PANEL_TEST_IPS
    err = esp_lcd_new_panel_ili9488_ips(p.io, &cfg, convert_px, &p.handle);
#else
    err = esp_lcd_new_panel_ili9488(p.io, &cfg, convert_px, &p.handle);
#endif
    if (err != ESP_OK) return err;

    if ((err = esp_lcd_panel_reset(p.handle)) != ESP_OK) return err;
    if ((err = esp_lcd_panel_init(p.handle)) != ESP_OK) return err;
    // The firmware's landscape setting (drivers/src/ili9488.cpp). The
    // corners test shows whether it is right for this panel.
    esp_lcd_panel_swap_xy(p.handle, true);
    esp_lcd_panel_mirror(p.handle, true, false);
    return esp_lcd_panel_disp_on_off(p.handle, true);
}

// One band out, then wait for its DMA. The panel plays no part in "done":
// a timeout means the ESP side is misconfigured, not that the panel is dead.
bool blit(Panel& p, int y0) {
    xSemaphoreTake(p.done, 0);
    if (esp_lcd_panel_draw_bitmap(p.handle, 0, y0, kW, y0 + kBandRows,
                                  p.band) != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed at row %d", y0);
        return false;
    }
    if (xSemaphoreTake(p.done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "band at row %d never finished: DMA or bus config", y0);
        return false;
    }
    return true;
}

template <typename ColorAt>
bool draw(Panel& p, ColorAt color_at) {
    for (int y0 = 0; y0 < kH; y0 += kBandRows) {
        for (int r = 0; r < kBandRows; ++r)
            for (int x = 0; x < kW; ++x)
                p.band[r * kW + x] = p.wire(color_at(x, y0 + r));
        if (!blit(p, y0)) return false;
    }
    return true;
}

void hold() { vTaskDelay(pdMS_TO_TICKS(kHoldMs)); }

void test_solids(Panel& p) {
    struct { uint16_t c; const char* name; } const fills[] = {
        {kRed, "RED"}, {kGreen, "GREEN"}, {kBlue, "BLUE"},
        {kWhite, "WHITE"}, {kBlack, "BLACK"},
    };
    for (const auto& f : fills) {
        ESP_LOGI(TAG, "[1/5 solid] expect a flat %s screen", f.name);
        draw(p, [&](int, int) { return f.c; });
        hold();
    }
}

// Three horizontal ramps, dark on the left to full on the right: red on
// top, green in the middle, blue at the bottom. On the 8-bit bus pixel bit
// k travels on line D(k % 8), and every line carries a high-order bit of
// some channel, so any dead or swapped line breaks a ramp visibly. The
// line-by-line table is in README.md.
void test_ramps(Panel& p) {
    ESP_LOGI(TAG, "[2/5 ramps] expect smooth red / green / blue ramps, "
                  "dark left to bright right, no jumps or repeats");
    draw(p, [](int x, int y) -> uint16_t {
        const int band = y * 3 / kH;
        if (band == 0) return static_cast<uint16_t>((x * 31 / (kW - 1)) << 11);
        if (band == 1) return static_cast<uint16_t>((x * 63 / (kW - 1)) << 5);
        return static_cast<uint16_t>(x * 31 / (kW - 1));
    });
    hold();
}

// Addressing and orientation: a 2 px white border on all four edges, then
// red in the top-left corner, green top-right, blue bottom-left, and
// nothing bottom-right.
void test_corners(Panel& p) {
    ESP_LOGI(TAG, "[3/5 corners] expect white border on all 4 edges; RED "
                  "top-left, GREEN top-right, BLUE bottom-left, bottom-right "
                  "empty");
    constexpr int kBox = 48;
    draw(p, [](int x, int y) -> uint16_t {
        if (x < 2 || y < 2 || x >= kW - 2 || y >= kH - 2) return kWhite;
        const bool left = x < kBox, right = x >= kW - kBox;
        const bool top = y < kBox, bottom = y >= kH - kBox;
        if (top && left) return kRed;
        if (top && right) return kGreen;
        if (bottom && left) return kBlue;
        return kBlack;
    });
    hold();
}

// Worst case for the wires: 0x55AA goes out as 0x55 then 0xAA, so every
// data line flips on every WR strobe. The screen should be one flat
// grey-green. Speckle or shimmer is signal integrity — shorter wires, a
// common ground, or a lower clock.
void test_toggle(Panel& p) {
    ESP_LOGI(TAG, "[4/5 toggle] expect one flat grey-green, no speckle "
                  "(every data line flips every write)");
    draw(p, [](int, int) -> uint16_t { return 0x55AA; });
    hold();
}

void test_throughput(Panel& p) {
    ESP_LOGI(TAG, "[5/5 timing] %d full frames, black/white alternating",
             kTimedFrames);
    int64_t total_us = 0;
    for (int f = 0; f < kTimedFrames; ++f) {
        const uint16_t c = p.wire((f & 1) ? kWhite : kBlack);
        for (int i = 0; i < kBandPx; ++i) p.band[i] = c;
        const int64_t t0 = esp_timer_get_time();
        for (int y0 = 0; y0 < kH; y0 += kBandRows)
            if (!blit(p, y0)) return;
        total_us += esp_timer_get_time() - t0;
    }
    const double ms = total_us / 1000.0 / kTimedFrames;
    // Wire-only floor: 2 bytes per pixel, one byte per WR strobe on i80;
    // 3 bytes per pixel, one bit per clock on SPI.
    const double floor_ms = kI80
        ? (kW * kH * 2.0) / kPclkHz * 1000.0
        : (kW * kH * 3.0 * 8.0) / kPclkHz * 1000.0;
    ESP_LOGI(TAG, "frame: %.1f ms (%.1f fps) at %d MHz; wire floor %.1f ms",
             ms, 1000.0 / ms, CONFIG_PANEL_TEST_PCLK_MHZ, floor_ms);
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(TAG, "ILI9488 panel test: %s at %d MHz, %dx%d landscape",
             kI80 ? "8-bit 8080, RGB565" : "4-line SPI, RGB666",
             CONFIG_PANEL_TEST_PCLK_MHZ, kW, kH);

    Panel p;
    const esp_err_t err = bring_up(p);
    if (err != ESP_OK) {
        // Nothing to retry: this is config or memory, not the panel.
        ESP_LOGE(TAG, "bring-up failed: %s — check the pins in menuconfig",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "init sent. The panel cannot acknowledge it: watch the "
                  "screen, not this log.");

    for (int pass = 1;; ++pass) {
        ESP_LOGI(TAG, "---- pass %d ----", pass);
        test_solids(p);
        test_ramps(p);
        test_corners(p);
        test_toggle(p);
        test_throughput(p);
    }
}
