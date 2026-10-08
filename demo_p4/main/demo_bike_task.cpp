#include "demo_bike_task.hpp"
#include "services/logic_tick.hpp"
#include "ble_media.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

namespace demo {
namespace {

constexpr uint32_t kTickMs = 20;   // 50 Hz, matching sim/sim_main.cpp

class QueueCommandSource final : public services::ICommandSource {
public:
    explicit QueueCommandSource(services::CommandQueue& q) : q_(q) {}
    bool take(core::state::Command& out) override { return q_.take(out); }

private:
    services::CommandQueue& q_;
};

void bike_task(void* arg) {
    auto* pair = static_cast<std::pair<services::Deps*, services::CommandQueue*>*>(arg);
    services::Deps& d = *pair->first;
    services::CommandQueue& q = *pair->second;

    // No made-up ride: this board has no bike sensors, so the slots for
    // engine, motion, attitude and GPS are never written and every screen
    // shows what the real dash shows with nothing connected -- zeros and
    // `--`. That also leaves the start-up gauge sweep free to play.
    services::LogicState logic;
    services::logic_restore(d, logic);
    QueueCommandSource commands(q);

    uint32_t now = d.clock->now_ms();
    // generation(), not the value itself: media_ctrl has no way to tell "a
    // new one arrived" from "still the last one" other than this, since it
    // is deliberately not a queue (see core::state::MediaCtrl).
    uint32_t ctrl_gen = 0;

    for (;;) {
        now += kTickMs;
        // The single write of media — ble_media.cpp's own callback never
        // touches the slot directly, only this cache (rule 2: one writer).
        d.slots->media.publish(demo::ble_media_snapshot());
        d.slots->ble_diag.publish(demo::ble_media_diag());

        services::logic_tick(d, logic, commands, now);

        // Ctrl: whatever the rider just pressed on the speed screen, one
        // BLE notify per press. logic_tick is the one writer of this slot
        // (§4.5's rule 2); this is just the one task that already loops
        // continuously and already has ble_media in scope to hand it off to.
        const uint32_t gen = d.slots->media_ctrl.generation();
        if (gen != ctrl_gen) {
            ctrl_gen = gen;
            demo::ble_media_send_ctrl(d.slots->media_ctrl.get().cmd);
        }

        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

std::pair<services::Deps*, services::CommandQueue*> g_bike_arg;

}  // namespace

void start_demo_bike(services::Deps& d, services::CommandQueue& q) {
    g_bike_arg = {&d, &q};
    xTaskCreatePinnedToCore(bike_task, "bike", 4096, &g_bike_arg, 5, nullptr, 1);
}

}  // namespace demo
