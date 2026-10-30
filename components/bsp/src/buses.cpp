#include "bsp/buses.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"

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

// The bus handles. bsp owns the buses, so it is the one place that remembers
// them: that is what keeps init idempotent (the i2c_master driver refuses a
// second bus on a port instead of saying "already up") and lets the pod's
// recovery take I2C0 down and back without anyone else holding a stale
// handle. Nothing outside this file writes them.
i2c_master_bus_handle_t s_i2c[2] = {nullptr, nullptr};

void* init_i2c(i2c_port_num_t port, int sda, int scl) {
    if (s_i2c[port]) return s_i2c[port];

    // The driver logs every NACK and timeout from inside the transfer. The
    // LED driver's writes are made from `ctrl`, at 18 on core 0, which must
    // never log (a line to the console is milliseconds of UART), and a dead
    // part is retried every 50 ms. Both drivers return the failure, and the
    // health table is where it shows.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = port;
    cfg.sda_io_num = static_cast<gpio_num_t>(sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(scl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    // Synchronous: each transfer returns when it is on the wire or failed.
    cfg.trans_queue_depth = 0;
    // The external resistors do the work. The internal pull-ups are around
    // 45k and barely register; they are left on as insurance against an
    // unpopulated resistor, which is a real bring-up failure.
    cfg.flags.enable_internal_pullup = 1;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) return nullptr;
    s_i2c[port] = bus;
    return bus;
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

    // The driver's interrupt moves bytes from the hardware FIFO to the ring
    // buffer `gnss` waits on only when the FIFO holds 120 of them or the line
    // has been idle for 10 characters. RMC and GGA come back to back, so the
    // end of the first sentence of a fix could sit in the FIFO for ~20 ms at
    // 57600, by an amount that moved with the sentence lengths, and that is
    // when the fix is stamped. 8 bytes is 1.4 ms at 57600, and a couple of
    // hundred interrupts a second of a few microseconds each.
    if (uart_set_rx_full_threshold(UART_NUM_1, kGpsRxFullBytes) != ESP_OK) return false;
    if (uart_set_rx_timeout(UART_NUM_1, kGpsRxIdleChars) != ESP_OK) return false;

    return uart_set_pin(UART_NUM_1, kGpsTx, kGpsRx, UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE) == ESP_OK;
}

}  // namespace

void* init_lcd_bus(bool swap_wr_dc) {
    esp_lcd_i80_bus_config_t bus = {};
    bus.clk_src = LCD_CLK_SRC_DEFAULT;
    bus.dc_gpio_num = static_cast<gpio_num_t>(swap_wr_dc ? kTftWr : kTftDc);
    bus.wr_gpio_num = static_cast<gpio_num_t>(swap_wr_dc ? kTftDc : kTftWr);
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

void release_lcd_bus(void* bus) {
    if (bus) esp_lcd_del_i80_bus(static_cast<esp_lcd_i80_bus_handle_t>(bus));
}

BusStatus init_buses() {
    BusStatus s;
    s.spi2 = init_spi2();
    s.i2c0 = init_i2c(0, kI2cSda, kI2cScl);
    s.i2c1 = init_i2c(1, kLedSda, kLedScl);
    s.uart1 = init_uart1();
    return s;
}

void* init_pod_bus() {
    return init_i2c(0, kI2cSda, kI2cScl);
}

void release_pod_bus() {
    if (s_i2c[0] && i2c_del_master_bus(s_i2c[0]) == ESP_OK) s_i2c[0] = nullptr;
}

BusStatus board_init() {
    // Nothing on this board draws current from a floating pin any more. The
    // shift lamps and the backlight are on the PCA9955B, whose outputs come
    // out of power-on reset with zero current and zero duty, and the panel's
    // reset and chip select are held inactive by R3 and R21.
    BusStatus s = init_buses();
    s.lcd_bus = init_lcd_bus(false);
    return s;
}

}  // namespace bsp
