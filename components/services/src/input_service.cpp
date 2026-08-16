// `input` — core 0, priority 9, 100 Hz.
//
// The pod has no interrupt line, so it is polled. Debounce and press
// decoding are in core/input, host-tested, because "does a 700 ms press give
// one long event or four short ones" should not need a bike.
//
// This task also carries the §12 rule about the pod specifically: short
// timeouts, count the NACKs, and on a run of failures re-write the
// configuration registers. A browned-out MCP23017 answers every read with
// "no buttons pressed" and looks perfectly healthy.
#include "services/services.hpp"
#include "core/input/input.hpp"
#include "drivers/pod.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr uint32_t kRefreshEveryMs = 5000;
constexpr int      kNacksBeforeRecover = 20;

void input_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::input::Decoder decoder;
    core::input::Config  cfg;
    int      consecutive_errors = 0;
    uint32_t last_refresh = 0;

    for (;;) {
        const uint32_t now = d.clock->now_ms();

        uint8_t mask = 0;
        const hal::Status s = d.pod ? d.pod->read_buttons(mask)
                                    : hal::Status::NotPresent;

        if (hal::ok(s)) {
            consecutive_errors = 0;
            d.health->report_ok(core::state::Dev::McpPod, now);

            core::nav::InputEvent ev[core::input::kButtonCount];
            const size_t n = decoder.update(mask, now, cfg, ev,
                                            core::input::kButtonCount);
            for (size_t i = 0; i < n; ++i) post_input_event(ev[i]);
        } else {
            ++consecutive_errors;
            d.health->report_error(core::state::Dev::McpPod,
                                   static_cast<int32_t>(s), now);
            // A button stuck down while the cable is unplugged would repeat
            // forever otherwise.
            decoder.reset();
        }

        if (auto* pod = static_cast<drivers::ButtonPod*>(d.pod)) {
            if (consecutive_errors >= kNacksBeforeRecover) {
                consecutive_errors = 0;
                pod->recover_bus();
            } else if (now - last_refresh >= kRefreshEveryMs) {
                // Periodic, not just on failure: a pod that came back with
                // default registers is not failing anything.
                last_refresh = now;
                pod->refresh_config();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));      // 100 Hz
    }
}

}  // namespace

void start_input(Deps& d, CommandQueue&) {
    xTaskCreatePinnedToCore(input_task, "input", 3072, &d, 9, nullptr, 0);
}

}  // namespace services
