// The five buttons over the USB serial port, as on the demo
// (demo_p4/main/serial_buttons.cpp): one line per press, `btn A`..`btn E`
// short, `long A`..`long E` long; or, from a terminal that reports key
// releases, `hold <mask>`, the buttons held, which `input` debounces and
// decodes like the pod's and which can play Doom. For the bench while the pod is not fitted;
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_attr.h"

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

// `screen`: the glass, one text line a row, so a log line from another
// task can land between two rows but never inside one (each is one write).
//
//   SCREEN BEGIN <w> <h> <seq>
//   SCR <y> <base64 of (u16 count, u16 rgb565) runs, little-endian>
//   SCREEN END <seq>
//
// tools/dash_screen.py turns them into a PNG. Mostly flat colour, so the
// runs make a frame a few kB to a few tens of kB.
EXT_RAM_BSS_ATTR uint8_t g_rle[ui::kScreenW * 4];
EXT_RAM_BSS_ATTR char    g_line[ui::kScreenW * 4 * 4 / 3 + 32];

size_t base64(const uint8_t* in, size_t n, char* out) {
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (i + 1 < n ? uint32_t(in[i + 1]) << 8 : 0) |
                           (i + 2 < n ? in[i + 2] : 0);
        out[k++] = kB64[(v >> 18) & 63];
        out[k++] = kB64[(v >> 12) & 63];
        out[k++] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        out[k++] = i + 2 < n ? kB64[v & 63] : '=';
    }
    return k;
}

void send_screen(const services::Deps& d) {
    static uint32_t seq = 0;
    const ui::Color* glass = d.glass.load(std::memory_order_acquire);
    if (!glass) {
        std::printf("SCREEN NONE\n");
        return;
    }
    ++seq;
    std::printf("SCREEN BEGIN %d %d %u\n", ui::kScreenW, ui::kScreenH, static_cast<unsigned>(seq));
    std::fflush(stdout);
    for (int y = 0; y < ui::kScreenH; ++y) {
        const ui::Color* row = glass + static_cast<size_t>(y) * ui::kScreenW;
        size_t n = 0;
        for (int x = 0; x < ui::kScreenW;) {
            const ui::Color c = row[x];
            int run = 1;
            while (x + run < ui::kScreenW && row[x + run] == c) ++run;
            g_rle[n++] = static_cast<uint8_t>(run);
            g_rle[n++] = static_cast<uint8_t>(run >> 8);
            g_rle[n++] = static_cast<uint8_t>(c);
            g_rle[n++] = static_cast<uint8_t>(c >> 8);
            x += run;
        }
        const int head = std::snprintf(g_line, sizeof g_line, "SCR %d ", y);
        size_t k = static_cast<size_t>(head) + base64(g_rle, n, g_line + head);
        g_line[k++] = '\n';
        std::fwrite(g_line, 1, k, stdout);       // one write: one line
        std::fflush(stdout);
    }
    std::printf("SCREEN END %u\n", static_cast<unsigned>(seq));
    std::fflush(stdout);
}

struct ConsoleArg {
    ui::ICommandSink* commands;
    services::Deps*   deps;
};
ConsoleArg g_console_arg;

void handle(const char* line, ui::ICommandSink& commands, const services::Deps& deps) {
    if (c6_line(line)) return;
    if (std::strcmp(line, "screen") == 0) {
        send_screen(deps);
        return;
    }
    // `usb drive`: the card to the laptop as a USB drive (parked; logic
    // decides). The console goes quiet until it comes back: unplug, or
    // CONFIG -> SD card -> USB drive again (drivers::UsbDrive).
    if (std::strcmp(line, "usb drive") == 0) {
        ESP_LOGI(kTag, "usb drive: asking (parked only)");
        commands.post(core::state::Command(core::state::CommandId::StorageToHost));
        return;
    }
    // `crash test`: the crash countdown, then the SOS to the phone (core::crash).
    if (std::strcmp(line, "crash test") == 0) {
        ESP_LOGW(kTag, "crash test: countdown started");
        commands.post(core::state::Command(core::state::CommandId::CrashTest));
        return;
    }
    // `panic`: crash on purpose, for bring-up: the core dump, the reboot,
    // and a dev image falling back to stable (bringup §2, §9.1 step 4).
    if (std::strcmp(line, "panic") == 0) {
        ESP_LOGE(kTag, "panic requested over USB");
        abort();
    }
    // `route bench`: time a statewide route through the card (navigation §9).
    if (std::strcmp(line, "route bench") == 0) {
        ESP_LOGI(kTag, "route bench: on storage's next pass");
        services::bench_route();
        return;
    }
    // `odo <miles>`, `hours <h>`: set the odometer or the engine hours, after
    // an NVS erase or a board swap. Here and nowhere on the dash: it is not
    // a thing to reach with a thumb.
    if (std::strncmp(line, "odo ", 4) == 0 || std::strncmp(line, "hours ", 6) == 0) {
        const bool odo = line[0] == 'o';
        const char* arg = line + (odo ? 4 : 6);
        char* end = nullptr;
        const float v = std::strtof(arg, &end);
        if (end == arg || !(v >= 0.0f)) {
            ESP_LOGW(kTag, "usage: odo <miles> | hours <h>");
        } else {
            ESP_LOGW(kTag, "%s set to %.2f", odo ? "odometer" : "engine hours",
                     static_cast<double>(v));
            commands.post(core::state::Command(odo ? core::state::CommandId::OdoSet
                                                   : core::state::CommandId::EngHoursSet, v));
        }
        return;
    }
    // `tpms sniff [dBm]`: -70 by default; -100 for everything, or a C6
    // without its antenna, which hears everything weak.
    if (std::strncmp(line, "tpms sniff", 10) == 0 && (line[10] == '\0' || line[10] == ' ')) {
        const long dbm = line[10] ? std::strtol(line + 11, nullptr, 10) : -70;
        const int8_t floor_dbm = static_cast<int8_t>(dbm < -127 ? -127 : dbm > 0 ? 0 : dbm);
        ESP_LOGI(kTag, "sniffing BLE adverts at %d dBm or nearer for 60 s", floor_dbm);
        services::sniff_tpms(60000, floor_dbm);
        return;
    }
    if (std::strcmp(line, "pod trace") == 0) {
        ESP_LOGI(kTag, "tracing the pod for 30 s");
        services::trace_pod(30000);
        return;
    }
    // `hold <0-31>`: the buttons held on the laptop, bit 0 = A, sent on
    // every change and repeated while any is down (pod_keys.py on a
    // terminal that reports key releases). Decoded as the pod's are.
    if (std::strncmp(line, "hold ", 5) == 0) {
        const long m = std::strtol(line + 5, nullptr, 10);
        if (m >= 0 && m < 32) services::set_console_buttons(static_cast<uint8_t>(m));
        return;
    }
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

void serial_task(void* arg) {
    const ConsoleArg& a = *static_cast<ConsoleArg*>(arg);
    ui::ICommandSink& commands = *a.commands;
    char line[32];
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        // A line is typed or sent whole; half of one, left by a sender that
        // stopped mid-write, would otherwise be glued onto the next
        // session's first command and spoil it. So silence drops it.
        if (usb_serial_jtag_read_bytes(&ch, 1, n ? pdMS_TO_TICKS(500) : portMAX_DELAY) != 1) {
            n = 0;
            continue;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[n] = '\0';
            handle(line, commands, *a.deps);
            n = 0;
        } else if (n + 1 < sizeof line) {
            line[n++] = static_cast<char>(ch);
        }
    }
}

}  // namespace

void start_serial_buttons(ui::ICommandSink& commands, services::Deps& deps) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "no USB serial driver; serial buttons off");
        return;
    }
    // The console is this port too: send it through the driver now, not
    // straight at the FIFO under the driver's feet.
    usb_serial_jtag_vfs_use_driver();
    g_console_arg = {&commands, &deps};
    xTaskCreatePinnedToCore(serial_task, "serial_btn", 3072, &g_console_arg, 3, nullptr, 1);
}
