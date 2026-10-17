#include "drivers/engine_sense.hpp"
#include "bsp/pins.hpp"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

namespace drivers {
namespace {

// One of these per channel. Written only by its ISR, read by `ctrl`.
//
// The pair is deliberately not a struct copied as a unit: on a 32-bit part
// two aligned 32-bit loads are each atomic, but not together. An edge
// landing between them paired one edge's count with another's timestamp,
// which read as half the rpm for one tick, a lamp dropping out for 2 ms.
// So the reader takes the count on both sides of the timestamp and goes
// again if it moved (read_channel). The ISR runs on core 0 with `ctrl`, so
// it can only fall between the reader's loads, never part-way through
// its own stores.
struct Channel {
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> edge_us_lo{0};
    uint32_t              last_edge_us = 0;   // ISR-private
    uint32_t              refractory_us = 0;
};

Channel g_tach;
Channel g_front;
Channel g_rear;

// Who to wake on a tach edge; see EngineSense::notify_on_tach.
std::atomic<TaskHandle_t> g_tach_waiter{nullptr};

// The ISR is the one place in this firmware with global state, and it has to
// be: a GPIO interrupt handler takes a void* and nothing else. Keeping it to
// three counters, written by nobody else, is the narrowest version of that
// compromise — it is not a `globals.h` coming back.
void IRAM_ATTR edge_isr(void* arg) {
    Channel* ch = static_cast<Channel*>(arg);
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());

    // Ringing rejection. Without a filter cap on the tach input, one spark
    // rings for tens of microseconds and every ring is an edge. Anything
    // inside the window is the same event.
    if (ch->refractory_us && (now - ch->last_edge_us) < ch->refractory_us) return;

    ch->last_edge_us = now;
    ch->edge_us_lo.store(now, std::memory_order_relaxed);
    ch->count.fetch_add(1, std::memory_order_release);

    if (ch == &g_tach) {
        if (TaskHandle_t t = g_tach_waiter.load(std::memory_order_relaxed)) {
            BaseType_t woke = pdFALSE;
            vTaskNotifyGiveFromISR(t, &woke);
            if (woke) portYIELD_FROM_ISR();
        }
    }
}

// One consistent (count, timestamp) pair. Three tries is two edges landing
// inside a window of a few nanoseconds, twice; the last try is kept anyway.
void read_channel(const Channel& ch, uint32_t& count, uint32_t& edge_lo) {
    for (int i = 0; i < 3; ++i) {
        count = ch.count.load(std::memory_order_acquire);
        edge_lo = ch.edge_us_lo.load(std::memory_order_relaxed);
        if (ch.count.load(std::memory_order_acquire) == count) return;
    }
}

// Rebuild a 64-bit timestamp from the 32-bit one the ISR stored. The low
// word wraps every 71 minutes; `now` is never more than a few milliseconds
// ahead of the edge, so the high word is either the current one or one less.
uint64_t widen(uint32_t lo, uint64_t now_us) {
    const uint64_t hi = now_us & 0xFFFFFFFF00000000ull;
    uint64_t v = hi | lo;
    if (v > now_us) v -= 0x100000000ull;
    return v;
}

hal::Status install(int pin, Channel& ch, uint32_t refractory_us) {
    ch.refractory_us = refractory_us;

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ull << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_POSEDGE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return hal::Status::IoError;

    err = gpio_isr_handler_add(static_cast<gpio_num_t>(pin), edge_isr, &ch);
    return err == ESP_OK ? hal::Status::Ok : hal::Status::IoError;
}

}  // namespace

void EngineSense::notify_on_tach(void* task) {
    g_tach_waiter.store(static_cast<TaskHandle_t>(task), std::memory_order_relaxed);
}

hal::Status EngineSense::init() {
    if (!started_) {
        // IRAM so an edge is still counted while flash is busy — the logger
        // writing to the card must not cost the rider revs.
        const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return hal::Status::IoError;
        started_ = true;
    }

    if (tach_refractory_us_ == 0) tach_refractory_us_ = bsp::kTachRefractoryUs;

    hal::Status s = install(bsp::kTachSig, g_tach, tach_refractory_us_);
    if (!hal::ok(s)) return s;
    // BRINGUP: the wheel Halls are clean digital signals through their own
    // conditioning, so they get a short window against contact chatter only.
    // Confirm on the bench with a wheel on a stand before trusting the mph.
    s = install(bsp::kWheelHallFront, g_front, 200);
    if (!hal::ok(s)) return s;
    s = install(bsp::kWheelHallRear, g_rear, 200);
    if (!hal::ok(s)) return s;

    // Neutral. R64 is the pull-up; the internal one is insurance against
    // it being unpopulated, the same reasoning as the I2C pull-ups.
    gpio_config_t n = {};
    n.pin_bit_mask = 1ull << bsp::kNeutral;
    n.mode = GPIO_MODE_INPUT;
    n.pull_up_en = GPIO_PULLUP_ENABLE;
    n.pull_down_en = GPIO_PULLDOWN_DISABLE;
    n.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&n) != ESP_OK) return hal::Status::IoError;
    ready_ = true;
    return hal::Status::Ok;
}

hal::Status EngineSense::read(Counts& out) {
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

    uint32_t lo = 0;
    read_channel(g_tach, out.tach_pulses, lo);
    out.tach_edge_us = widen(lo, now);
    read_channel(g_front, out.front_pulses, lo);
    out.front_edge_us = widen(lo, now);
    read_channel(g_rear, out.rear_pulses, lo);
    out.rear_edge_us = widen(lo, now);
    out.t_us = now;

    // Neutral is a plain GPIO on this board. It reads LOW in neutral —
    // opto-isolated and pulled up — and is inverted here so that nothing
    // above ever has to remember that.
    const int level = gpio_get_level(static_cast<gpio_num_t>(bsp::kNeutral));
    out.neutral = bsp::kNeutralActiveLow ? level == 0 : level != 0;
    // Counters that were never hooked up read zero, and zero looks like a
    // parked bike. Say so, so that `ctrl` does not trust the wheels.
    return ready_ ? hal::Status::Ok : hal::Status::NotPresent;
}

}  // namespace drivers
