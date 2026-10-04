#include "bsp/buses.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_lcd_io_i80.h"

namespace bsp {
namespace {

// ESP_ERR_INVALID_STATE from any of these means "already up", which is a
// success for an idempotent init.
bool ok_or_already(esp_err_t err) {
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

bool init_spi2() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = static_cast<gpio_num_t>(kSpiMosi);
    bus.miso_io_num = static_cast<gpio_num_t>(kSpiMiso);
    bus.sclk_io_num = static_cast<gpio_num_t>(kSpiSclk);
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    // The IMU FIFO burst is the biggest transfer on this bus; the SD driver
    // asks for its own when it mounts.
    bus.max_transfer_sz = 4096;
    return ok_or_already(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
}

bool init_i2c(i2c_port_t port, int sda, int scl, int hz) {
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = static_cast<gpio_num_t>(sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(scl);
    // The external resistors do the work. The internal pull-ups are around
    // 45k and barely register; they are left on as insurance against an
    // unpopulated resistor, which is a real bring-up failure.
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = hz;

    if (i2c_param_config(port, &cfg) != ESP_OK) return false;
    return ok_or_already(i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0));
}

bool init_uart1() {
    uart_config_t cfg = {};
    cfg.baud_rate = kGpsBootBaud;      // the receiver powers up here
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    if (!ok_or_already(uart_driver_install(UART_NUM_1, 2048, 0, 0, nullptr, 0))) {
        return false;
    }
    if (uart_param_config(UART_NUM_1, &cfg) != ESP_OK) return false;
    return uart_set_pin(UART_NUM_1, kGpsTx, kGpsRx, UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE) == ESP_OK;
}

void* init_lcd_bus() {
    esp_lcd_i80_bus_config_t bus = {};
    bus.clk_src = LCD_CLK_SRC_DEFAULT;
    bus.dc_gpio_num = static_cast<gpio_num_t>(kTftDc);
    bus.wr_gpio_num = static_cast<gpio_num_t>(kTftWr);
    for (int i = 0; i < 8; ++i) {
        bus.data_gpio_nums[i] = static_cast<gpio_num_t>(kTftData[i]);
    }
    bus.bus_width = 8;
    bus.max_transfer_bytes = static_cast<size_t>(kTftMaxBlitPx) * 2;
    // The driver's own default, written out because it sets the DMA's
    // alignment rule: on the P4's AXI DMA every transfer's address and
    // length must be a multiple of the burst. drivers/ili9488 stages
    // through a buffer that honours it, so the UI never has to.
    bus.dma_burst_size = 32;

    esp_lcd_i80_bus_handle_t handle = nullptr;
    if (esp_lcd_new_i80_bus(&bus, &handle) != ESP_OK) return nullptr;
    return handle;
}

}  // namespace

BusStatus init_buses() {
    BusStatus s;
    s.spi2 = init_spi2();
    s.i2c0 = init_i2c(I2C_NUM_0, kI2cSda, kI2cScl, kI2cHz);
    s.i2c1 = init_i2c(I2C_NUM_1, kLedSda, kLedScl, kLedI2cHz);
    s.uart1 = init_uart1();
    return s;
}

BusStatus board_init() {
    // Nothing on this board draws current from a floating pin any more. The
    // shift lamps and the backlight are on the PCA9955B, whose outputs come
    // out of power-on reset with zero current and zero duty, and the panel's
    // reset and chip select are held inactive by R3 and R41.
    BusStatus s = init_buses();
    s.lcd_bus = init_lcd_bus();
    return s;
}

}  // namespace bsp
