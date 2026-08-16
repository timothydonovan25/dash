// `logic` — core 0, priority 10, 50 Hz.
//
// A loop and a delay. Everything it decides lives in `logic_tick.hpp`, which
// has no FreeRTOS in it and is therefore the same code the host simulator
// runs — see the note there on why that split matters.
#include "services/logic_tick.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

namespace services {
namespace {

// Adapts the FreeRTOS queue to the transport-free interface logic_tick takes.
class QueueSource final : public ICommandSource {
public:
    explicit QueueSource(CommandQueue& q) : q_(q) {}
    bool take(core::state::Command& out) override { return q_.take(out); }

private:
    CommandQueue& q_;
};

// The logic task's memory. Two point traces and a lap history is far more
// than a task stack holds, so it lives here rather than in the task.
LogicState g_state;

void logic_task(void* arg) {
    auto* pair = static_cast<std::pair<Deps*, CommandQueue*>*>(arg);
    Deps& d = *pair->first;
    QueueSource commands(*pair->second);

    logic_restore(d, g_state);

    TickType_t next = xTaskGetTickCount();
    for (;;) {
        logic_tick(d, g_state, commands, d.clock->now_ms());
        vTaskDelayUntil(&next, pdMS_TO_TICKS(20));      // 50 Hz
    }
}

// Not shared state: xTaskCreate takes one void*, and this is where the two
// references live long enough to survive start_logic() returning. Written
// once, before the task exists, and read only by it.
std::pair<Deps*, CommandQueue*> g_logic_arg;

}  // namespace

void start_logic(Deps& d, CommandQueue& q) {
    g_logic_arg = {&d, &q};
    xTaskCreatePinnedToCore(logic_task, "logic", 4096, &g_logic_arg, 10,
                            nullptr, 0);
}

}  // namespace services
