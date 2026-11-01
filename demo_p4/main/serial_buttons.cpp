// The sim's button protocol, on the board: a test script on the laptop
// walks DIAG -> Update and CONFIG on real hardware without anyone touching
// the screen. Presses go into the same queue the touch zones and the real
// pod feed, so nothing downstream can tell them apart.
#include "serial_buttons.hpp"
#include "services/services.hpp"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "soc/hp_sys_clkrst_reg.h"
#include "soc/soc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace demo {
namespace {

constexpr const char* kTag = "serial_btn";

// The clock gates and resets the ROM's SHA engine needs, for chasing the
// P4 v1.x reset fault (components/p4_restart_fix/restart_fix.c).
void dump_crypto_regs() {
    const uint32_t clk25 = REG_READ(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG);
    const uint32_t soc1  = REG_READ(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG);
    const uint32_t rst2  = REG_READ(HP_SYS_CLKRST_HP_RST_EN2_REG);
    ESP_LOGI(kTag, "regs: reset %d, PERI_CLK_CTRL25 %08lx (sha %d), SOC_CLK_CTRL1 %08lx "
             "(crypto_sys %d), HP_RST_EN2 %08lx (rst_sha %d, rst_crypto %d)",
             static_cast<int>(esp_reset_reason()),
             static_cast<unsigned long>(clk25), (clk25 & HP_SYS_CLKRST_REG_CRYPTO_SHA_CLK_EN) != 0,
             static_cast<unsigned long>(soc1), (soc1 & HP_SYS_CLKRST_REG_CRYPTO_SYS_CLK_EN) != 0,
             static_cast<unsigned long>(rst2), (rst2 & HP_SYS_CLKRST_REG_RST_EN_SHA) != 0,
             (rst2 & HP_SYS_CLKRST_REG_RST_EN_CRYPTO) != 0);
}

void handle(const char* line, StateRequest& req) {
    if (std::strcmp(line, "regs") == 0) return dump_crypto_regs();
    if (std::strcmp(line, "state") == 0) {
        req.pending.store(true);
        return;
    }
    if (std::strcmp(line, "inputs") == 0) {
        req.trace_inputs_until_ms.store(
            static_cast<uint32_t>(esp_timer_get_time() / 1000) + 30000);
        ESP_LOGI(kTag, "tracing inputs for 30 s");
        return;
    }
    core::nav::Press press;
    if (std::strncmp(line, "btn ", 4) == 0)       press = core::nav::Press::Short;
    else if (std::strncmp(line, "long ", 5) == 0) press = core::nav::Press::Long;
    else return;
    const char c = line[press == core::nav::Press::Short ? 4 : 5];
    if (c < 'A' || c > 'E') return;
    const auto b = static_cast<core::nav::Button>(c - 'A');
    services::post_input_event(core::nav::InputEvent{b, press});
    ESP_LOGI(kTag, "%s %c", press == core::nav::Press::Short ? "short" : "long", c);
}

void serial_task(void* arg) {
    StateRequest& req = *static_cast<StateRequest*>(arg);
    char line[32];
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        // Half a line left by a sender that stopped mid-write is dropped
        // after a moment of silence, not glued onto the next command.
        if (usb_serial_jtag_read_bytes(&ch, 1, n ? pdMS_TO_TICKS(500) : portMAX_DELAY) != 1) {
            n = 0;
            continue;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[n] = '\0';
            handle(line, req);
            n = 0;
        } else if (n + 1 < sizeof line) {
            line[n++] = static_cast<char>(ch);
        }
    }
}

}  // namespace

void start_serial_buttons(StateRequest& req) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "no USB serial driver; serial buttons off");
        return;
    }
    // The console is this port too: send it through the driver now, not
    // straight at the FIFO under the driver's feet.
    usb_serial_jtag_vfs_use_driver();
    dump_crypto_regs();
    xTaskCreatePinnedToCore(serial_task, "serial_btn", 3072, &req, 3, nullptr, 1);
}

}  // namespace demo
