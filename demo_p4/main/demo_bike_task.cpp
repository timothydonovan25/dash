#include "demo_bike_task.hpp"
#include "services/logic_tick.hpp"
#include "services/settings_store.hpp"

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
    // `--`. That also leaves the start-up gauge sweep free to play. Media
    // and Ctrl belong to the production `ble` task now, not to this one.
    // Static, as `logic` keeps its own (logic_service.cpp): LogicState is
    // 5.8 kB since 1.26 (the splits' path buffer), and on this task's 4 kB
    // stack it overflowed it at start and the demo boot-looped back to its
    // stable image. One bike task, so it is this task's alone.
    static services::LogicState logic;
    services::logic_restore(d, logic);
    QueueCommandSource commands(q);

    uint32_t now = d.clock->now_ms();
    // keeper's settings half, at keeper's 1 Hz: the production code that
    // gets a CONFIG change into NVS.
    services::SettingsSaver saver;
    uint32_t last_save_ms = now;

    for (;;) {
        now += kTickMs;
        services::logic_tick(d, logic, commands, now);

        if (d.storage && now - last_save_ms >= 1000) {
            last_save_ms = now;
            if (saver.save_changed(*d.storage, *d.settings) > 0) {
                d.storage->commit();
            }
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
