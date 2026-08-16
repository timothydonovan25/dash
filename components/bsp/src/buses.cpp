#include "bsp/buses.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

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

bool init_spi3() {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = static_cast<gpio_num_t>(kTftSclk);
    bus.mosi_io_num = static_cast<gpio_num_t>(kTftMosi);
    bus.miso_io_num = static_cast<gpio_num_t>(kTftMiso);
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    // One band of 40 lines at three bytes per pixel. The panel driver splits
    // anything larger, and the split shows as tearing.
    bus.max_transfer_sz = kTftWidth * 40 * 3;
    return ok_or_already(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
}

bool init_i2c0() {
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = static_cast<gpio_num_t>(kI2cSda);
    cfg.scl_io_num = static_cast<gpio_num_t>(kI2cScl);
    // The 4.7k externals do the work. The internal pull-ups are around 45k
    // and barely register; they are left on as insurance against an
    // unpopulated resistor, which is a real bring-up failure.
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    // 100 kHz, not 400. Half a metre of cable and 4.7k pull-ups give about
    // 470 ns of rise time; fast mode allows 300 ns and standard allows 1000.
    cfg.master.clk_speed = kI2cHz;

    if (i2c_param_config(I2C_NUM_0, &cfg) != ESP_OK) return false;
    return ok_or_already(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));
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

}  // namespace

BusStatus init_buses() {
    BusStatus s;
    s.spi2 = init_spi2();
    s.spi3 = init_spi3();
    s.i2c0 = init_i2c0();
    s.uart1 = init_uart1();
    return s;
}

BusStatus board_init() {
    // The shift-lamp master FET and the backlight are the two pins that can
    // draw real current the moment they float. Both are driven low before
    // anything else, because the USB bench limit is not theoretical: eight
    // lamps plus the backlight plus the 3V3 rail is roughly 610 mA against a
    // 500 mA port, and that browns out the expander that holds TFT_RST.
    gpio_config_t out = {};
    out.pin_bit_mask = (1ull << kShiftMasterPwm) | (1ull << kTftBacklight);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level(static_cast<gpio_num_t>(kShiftMasterPwm), 0);
    gpio_set_level(static_cast<gpio_num_t>(kTftBacklight), 0);

    return init_buses();
}

}  // namespace bsp
