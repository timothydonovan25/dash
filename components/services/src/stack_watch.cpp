// Every task's stack headroom: the least free any task's stack has ever had
// (FreeRTOS's high-water mark, in bytes on IDF), once a second.
//
// The compile-time frame limit (cmake/dash_stack.cmake) sees one function
// at a time. This sees what a task actually used -- a deep call chain, an
// ESP-IDF or ESP-Hosted task we did not size -- and it sees it creep before
// it overflows, rather than after, as the demo's bike task did in 1.26.
// DIAG -> System shows the tightest one; the console gets every task's once
// at ten seconds of uptime and a warning whenever one falls under the line.
#include "services/services.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

#if !configUSE_TRACE_FACILITY
#error "stack_watch needs CONFIG_FREERTOS_USE_TRACE_FACILITY (sdkconfig.defaults)"
#endif

namespace services {
namespace {

constexpr const char* kTag = "stack";
// Under this a task is one deep call (a log line formats into ~1 kB of
// stack on its own) from overflowing.
constexpr uint32_t kWarnBytes = 768;
// More tasks than this and the rest are not looked at; the dash has ~30.
constexpr UBaseType_t kMaxTasks = 48;

// Static: 48 statuses is ~2 kB, and this runs on keeper's stack.
TaskStatus_t g_status[kMaxTasks];
uint32_t     g_warned_at[kMaxTasks];    // the low each warning was for, by slot
char         g_warned_name[kMaxTasks][configMAX_TASK_NAME_LEN];
bool         g_listed = false;

}  // namespace

void watch_stacks(ui::SystemInfo& sys, uint32_t now_ms) {
    const UBaseType_t n = uxTaskGetSystemState(g_status, kMaxTasks, nullptr);
    if (n == 0) return;

    const TaskStatus_t* low = nullptr;
    for (UBaseType_t i = 0; i < n; ++i) {
        const TaskStatus_t& t = g_status[i];
        if (!low || t.usStackHighWaterMark < low->usStackHighWaterMark) low = &t;

        // A warning for each task each time its low moves under the line
        // again, not once a second for as long as it stays there.
        if (t.usStackHighWaterMark < kWarnBytes) {
            UBaseType_t slot = kMaxTasks;
            for (UBaseType_t k = 0; k < kMaxTasks; ++k) {
                if (std::strncmp(g_warned_name[k], t.pcTaskName, sizeof g_warned_name[k]) == 0) {
                    slot = k;
                    break;
                }
                if (slot == kMaxTasks && g_warned_name[k][0] == '\0') slot = k;
            }
            if (slot < kMaxTasks &&
                (g_warned_name[slot][0] == '\0' || t.usStackHighWaterMark < g_warned_at[slot])) {
                std::strncpy(g_warned_name[slot], t.pcTaskName, sizeof g_warned_name[slot] - 1);
                g_warned_at[slot] = t.usStackHighWaterMark;
                ESP_LOGW(kTag, "%s: %u bytes of stack left at its lowest",
                         t.pcTaskName, static_cast<unsigned>(t.usStackHighWaterMark));
            }
        }
    }

    std::strncpy(sys.stack_low_task, low->pcTaskName, sizeof sys.stack_low_task - 1);
    sys.stack_low_task[sizeof sys.stack_low_task - 1] = '\0';
    sys.stack_low_free_b = low->usStackHighWaterMark;

    // Once, after boot has settled: every task, for bring-up.
    if (!g_listed && now_ms >= 10000) {
        g_listed = true;
        for (UBaseType_t i = 0; i < n; ++i) {
            ESP_LOGI(kTag, "%-16s %5u bytes free at its lowest, prio %u",
                     g_status[i].pcTaskName,
                     static_cast<unsigned>(g_status[i].usStackHighWaterMark),
                     static_cast<unsigned>(g_status[i].uxCurrentPriority));
        }
    }
}

}  // namespace services
