// The five buttons over the USB serial port, as on the demo
// (demo_p4/main/serial_buttons.cpp): one line per press, `btn A`..`btn E`
// short, `long A`..`long E` long. For the bench while the pod is not fitted;
// tools/pod_keys.py turns laptop keys into these lines. Presses go into the
// queue the pod feeds, after debounce, so nothing downstream can tell them
// apart. With no laptop on USB the task just waits.
#include "serial_buttons.hpp"
#include "services/services.hpp"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace {

constexpr const char* kTag = "serial_btn";

void handle(const char* line) {
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
