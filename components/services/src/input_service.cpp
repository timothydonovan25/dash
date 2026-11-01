// `input` — core 0, priority 9, 200 Hz (bsp::kPodPollHz).
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
#include "bsp/pins.hpp"
#include "drivers/pod.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr uint32_t kRefreshEveryMs = 5000;
// A run of failed reads this long, in time rather than in reads, before the
// bus is recovered: the poll rate is a board fact, and a pod unplugged
// should not be recovered twice as often because the rate went up.
constexpr uint32_t kRecoverAfterMs = 200;
constexpr int      kNacksBeforeRecover =
    static_cast<int>(kRecoverAfterMs * bsp::kPodPollHz / 1000);
// Recoveries that did not bring it back before the pod is taken as gone and
// only probed, once a second, until a read works. An absent pod was a 10 ms
// I2C timeout every 5 ms, the bus driver torn down five times a second, and
// a fault on every read, for the whole ride (rule 5: no tight retry).
constexpr int      kRecoveriesBeforeProbing = 3;
constexpr uint32_t kProbeEveryMs = 1000;

void input_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::input::Decoder decoder;
    core::input::Config  cfg;
    int      consecutive_errors = 0;
    int      failed_recoveries = 0;
    uint32_t next_probe = 0;
    uint32_t last_refresh = 0;
    TickType_t next = xTaskGetTickCount();

    constexpr TickType_t kPeriod = pdMS_TO_TICKS(1000 / bsp::kPodPollHz);
    for (;;) {
        const uint32_t now = d.clock->now_ms();

        if (failed_recoveries >= kRecoveriesBeforeProbing) {
            if (static_cast<int32_t>(now - next_probe) < 0) {
                vTaskDelayUntil(&next, kPeriod);
                if (xTaskGetTickCount() - next > kPeriod) next = xTaskGetTickCount();
                continue;
            }
            next_probe = now + kProbeEveryMs;
        }

        uint8_t mask = 0;
        const hal::Status s = d.pod ? d.pod->read_buttons(mask)
                                    : hal::Status::NotPresent;

        // A pod that has just come back (plugged in, powered up between
        // probes) answers with its power-on registers: no input inversion,
        // so every released button reads pressed, and five long presses
        // would follow. Configure it before believing it.
        bool fresh = false;
        if (hal::ok(s) && (consecutive_errors > 0 || failed_recoveries > 0)) {
            if (failed_recoveries >= kRecoveriesBeforeProbing)
                ESP_LOGI("input", "pod answering again: polling at %d Hz", bsp::kPodPollHz);
            if (auto* pod = static_cast<drivers::ButtonPod*>(d.pod)) pod->refresh_config();
            last_refresh = now;
            decoder.reset();
            fresh = true;
        }

        if (hal::ok(s)) {
            consecutive_errors = 0;
            failed_recoveries = 0;
            d.health->report_ok(core::state::Dev::McpPod, now);
            if (!fresh) {
                core::nav::InputEvent ev[core::input::kButtonCount];
                const size_t n = decoder.update(mask, now, cfg, ev,
                                                core::input::kButtonCount);
                for (size_t i = 0; i < n; ++i) post_input_event(ev[i]);
            }
        } else {
            ++consecutive_errors;
            d.health->report_error(core::state::Dev::McpPod,
                                   static_cast<int32_t>(s), now);
            // A button stuck down while the cable is unplugged would repeat
            // forever otherwise.
            decoder.reset();
        }

        if (auto* pod = static_cast<drivers::ButtonPod*>(d.pod)) {
            // This task is the only one that recovers the pod's bus, on its
            // own run of NACKs or when `keeper` asks: recovery deletes the
            // I2C driver, and from another core it did that under a read.
            const bool asked = take_retry(d, core::state::Dev::McpPod);
            // Probing, one read a second: recover on every failed probe
            // rather than after 40 of them.
            const bool probing = failed_recoveries >= kRecoveriesBeforeProbing;
            if (consecutive_errors >= kNacksBeforeRecover || asked ||
                (probing && consecutive_errors > 0)) {
                if (consecutive_errors > 0 && failed_recoveries < kRecoveriesBeforeProbing &&
                    ++failed_recoveries == kRecoveriesBeforeProbing)
                    ESP_LOGW("input", "pod not answering: probing it once a second");
                consecutive_errors = 0;
                pod->recover_bus();
            } else if (now - last_refresh >= kRefreshEveryMs) {
                // Periodic, not just on failure: a pod that came back with
                // default registers is not failing anything.
                last_refresh = now;
                pod->refresh_config();
            }
        }

        // From bsp, not a literal: the pod has no interrupt line, so this
        // rate is a board fact and core::input's windows are specified in
        // milliseconds so that changing it here does not silently change
        // what counts as a long press.
        //
        // Paced from a fixed schedule, not "10 ms after this pass": the
        // debounce counts on reads being one period apart.
        // After an overrun (a dead pod is 10 ms of timeout per read, two periods) start
        // again from now rather than catching up with a burst of reads.
        vTaskDelayUntil(&next, kPeriod);
        const TickType_t woke = xTaskGetTickCount();
        if (woke - next > kPeriod) next = woke;
    }
}

}  // namespace

void start_input(Deps& d, CommandQueue&) {
    xTaskCreatePinnedToCore(input_task, "input", 3072, &d, 9, nullptr, 0);
}

}  // namespace services
