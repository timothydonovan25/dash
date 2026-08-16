#include "services/services.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace services {
namespace {
// The input event queue. One producer (`input`), one consumer (`render`).
QueueHandle_t g_input_q = nullptr;
}  // namespace

bool CommandQueue::begin(size_t depth) {
    if (q_) return true;
    q_ = xQueueCreate(depth, sizeof(core::state::Command));
    if (!g_input_q) {
        g_input_q = xQueueCreate(16, sizeof(core::nav::InputEvent));
    }
    return q_ != nullptr && g_input_q != nullptr;
}

bool CommandQueue::post(const core::state::Command& c) {
    if (!q_) return false;
    // Never blocks. A UI task waiting on a full queue would be a dashboard
    // that stops drawing because the logic task is busy, which is the wrong
    // way round — rule 5 applied to our own code.
    if (xQueueSend(static_cast<QueueHandle_t>(q_), &c, 0) != pdTRUE) {
        ++dropped_;
        return false;
    }
    return true;
}

bool CommandQueue::post_from_isr(const core::state::Command& c) {
    if (!q_) return false;
    BaseType_t woke = pdFALSE;
    const BaseType_t ok =
        xQueueSendFromISR(static_cast<QueueHandle_t>(q_), &c, &woke);
    if (woke) portYIELD_FROM_ISR();
    if (ok != pdTRUE) ++dropped_;
    return ok == pdTRUE;
}

bool CommandQueue::take(core::state::Command& out) {
    if (!q_) return false;
    return xQueueReceive(static_cast<QueueHandle_t>(q_), &out, 0) == pdTRUE;
}

bool post_input_event(const core::nav::InputEvent& e) {
    if (!g_input_q) return false;
    return xQueueSend(g_input_q, &e, 0) == pdTRUE;
}

bool take_input_event(core::nav::InputEvent& out) {
    if (!g_input_q) return false;
    return xQueueReceive(g_input_q, &out, 0) == pdTRUE;
}

bool wait_input_event(core::nav::InputEvent& out, uint32_t timeout_ms) {
    if (!g_input_q) return false;
    return xQueueReceive(g_input_q, &out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

}  // namespace services
