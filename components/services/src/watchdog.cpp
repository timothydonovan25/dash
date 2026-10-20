// The task watchdog, for the tasks a rider would see stop.
//
// IDF's task watchdog only watched the two idle tasks, and on its own only
// printed, to a USB console nobody has plugged in on the bike. So a `logic`
// or `render` stuck in a loop froze the dash for the rest of the ride with
// nothing noticing. The tasks that join here (ctrl, imu, logic, render) feed
// it once a loop; one that has not fed it in CONFIG_ESP_TASK_WDT_TIMEOUT_S
// panics the board (CONFIG_ESP_TASK_WDT_PANIC, main.cpp insists), which is
// a core dump, a reboot of a second or two, and "task watchdog" as the
// reset reason in the next session's _evt file.
//
// This is system policy, not a driver deciding it (rule 5): no driver
// joins, and a driver still never aborts.
#include "services/services.hpp"

#include "esp_task_wdt.h"

namespace services {

void watchdog_join() {
    // Not fatal: without the watchdog the task runs as it did before.
    (void)esp_task_wdt_add(nullptr);
}

void watchdog_feed() {
    (void)esp_task_wdt_reset();
}

}  // namespace services
