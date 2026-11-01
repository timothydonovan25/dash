// The five buttons over the USB serial port, as on the demo
// (demo_p4/main/serial_buttons.cpp): one line per press, `btn A`..`btn E`
// short, `long A`..`long E` long. For the bench while the pod is not fitted;
// tools/pod_keys.py turns laptop keys into these lines. Presses go into the
// queue the pod feeds, after debounce, so nothing downstream can tell them
// apart. With no laptop on USB the task just waits.
#include "serial_buttons.hpp"
#include "services/services.hpp"

#include "bsp/pins.hpp"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace {

constexpr const char* kTag = "serial_btn";

// `c6 hold` / `c6 release`: the C6 held in reset, for testing what the dash
// does when the radio dies under it (bringup §9). ESP-Hosted owns the pin;
// this only overrides its level until the next boot or release.
bool c6_line(const char* line) {
    if (std::strncmp(line, "c6 ", 3) != 0) return false;
    // ESP-Hosted's "active" is the enable level (its reset pulse ends on
    // it): high on this module, whose GPIO54 is the C6's EN. Held is the
    // other one.
#if CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH
    constexpr int kHeld = 0;
#else
    constexpr int kHeld = 1;
#endif
    const auto pin = static_cast<gpio_num_t>(bsp::kC6Reset);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    const bool hold = std::strcmp(line + 3, "hold") == 0;
    if (!hold && std::strcmp(line + 3, "release") != 0) return true;
    gpio_set_level(pin, hold ? kHeld : !kHeld);
    ESP_LOGW(kTag, "C6 %s (GPIO%d = %d)", hold ? "held in reset" : "released",
             bsp::kC6Reset, hold ? kHeld : !kHeld);
    return true;
}

void handle(const char* line) {
    if (c6_line(line)) return;
    core::nav::Press press;
    if (std::strncmp(line, "btn ", 4) == 0)       press = core::nav::Press::Short;
    else if (std::strncmp(line, "long ", 5) == 0) press = core::nav::Press::Long;
    else return;
    const char c = line[press == core::nav::Press::Short ? 4 : 5];
    if (c < 'A' || c > 'E') return;
    services::post_input_event(
        core::nav::InputEvent{static_cast<core::nav::Button>(c - 'A'), press});
    ESP_LOGI(kTag, "%s %c", press == core::nav::Press::Short ? "short" : "long", c);
}

void serial_task(void*) {
    char line[32];
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[n] = '\0';
            handle(line);
            n = 0;
        } else if (n + 1 < sizeof line) {
            line[n++] = static_cast<char>(ch);
        }
    }
}

}  // namespace

void start_serial_buttons() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "no USB serial driver; serial buttons off");
        return;
    }
    // The console is this port too: send it through the driver now, not
    // straight at the FIFO under the driver's feet.
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(serial_task, "serial_btn", 3072, nullptr, 3, nullptr, 1);
}
