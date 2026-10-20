// `logic` — core 0, priority 10, 50 Hz and on demand.
//
// A loop and a wait. It ticks on a fixed 50 Hz schedule, and is woken
// between ticks by a task notification when there is news: a fix from
// `gnss` (wake_logic) or a command on the queue. A wake runs an Event pass
// at once and does not move the schedule, so the blackbox still logs on an
// even 20 ms grid. Everything it decides lives in `logic_tick.hpp`, which
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

    // Before anyone is told where to send wakes: a notification that arrives
    // before the first wait is kept, not lost, so nothing posted from here on
    // goes unseen for more than one tick.
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    pair->second->set_consumer(self);
    d.logic_task.store(self, std::memory_order_relaxed);

    constexpr TickType_t kPeriod = pdMS_TO_TICKS(20);    // 50 Hz
    TickType_t next = xTaskGetTickCount();
    watchdog_join();
    for (;;) {
        watchdog_feed();
        // Due already, or woken early? A wake that lands on the deadline
        // counts as the tick, so a busy receiver cannot starve the schedule.
        const TickType_t left = next - xTaskGetTickCount();
        if (static_cast<int32_t>(left) > 0) ulTaskNotifyTake(pdTRUE, left);
        const TickType_t now_ticks = xTaskGetTickCount();
        const bool due = static_cast<int32_t>(now_ticks - next) >= 0;

        if (due) {
            // Notifications that came in with the tick are served by it.
            ulTaskNotifyTake(pdTRUE, 0);
            next += kPeriod;
            // After an overrun -- a flash write froze the core -- start again
            // from now rather than running the missed ticks back to back.
            if (static_cast<int32_t>(now_ticks - next) >= 0) next = now_ticks + kPeriod;
        }
        logic_tick(d, g_state, commands, d.clock->now_ms(),
                   due ? LogicPass::Scheduled : LogicPass::Event);
    }
}

// Not shared state: xTaskCreate takes one void*, and this is where the two
// references live long enough to survive start_logic() returning. Written
// once, before the task exists, and read only by it.
std::pair<Deps*, CommandQueue*> g_logic_arg;

}  // namespace

void wake_logic(Deps& d) {
    if (void* t = d.logic_task.load(std::memory_order_relaxed)) {
        xTaskNotifyGive(static_cast<TaskHandle_t>(t));
    }
}

void start_logic(Deps& d, CommandQueue& q) {
    g_logic_arg = {&d, &q};
    xTaskCreatePinnedToCore(logic_task, "logic", 5120, &g_logic_arg, 10,
                            nullptr, 0);
}

}  // namespace services
