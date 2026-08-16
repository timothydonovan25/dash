// `ctrl` — core 0, priority 18, 500 Hz.
//
// The only latency-critical task on the bike. Tach edge to shift lamp has to
// stay under about 3 ms: one 500 Hz tick is 2 ms worst case, and one SPI
// write to the expander is about 20 us. Everything else here is bookkeeping
// that happens to share the loop.
//
// Nothing in this file is allowed to block, allocate, touch the filesystem
// or log. If a line here can wait, it belongs in `logic`.
#include "services/services.hpp"
#include "core/shift/shift.hpp"
#include "core/tach/tach.hpp"
#include "bsp/pins.hpp"
#include "drivers/mcp23s17.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr uint32_t kHz = 500;
constexpr TickType_t kPeriod = pdMS_TO_TICKS(2);

void ctrl_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::tach::Meter      tach;
    core::tach::WheelMeter front;
    core::tach::WheelMeter rear;
    uint8_t last_mask = 0;

    TickType_t next = xTaskGetTickCount();
    uint32_t late = 0;

    for (;;) {
        hal::IEngineSense::Counts c;
        const hal::Status s = d.sense ? d.sense->read(c) : hal::Status::NotPresent;

        core::tach::Config tc;
        tc.pulses_per_rev = 1;
        core::tach::WheelConfig fc;
        fc.circumference_mm = d.settings->front_circumference_mm;
        fc.pulses_per_rev   = d.settings->front_pulses_per_rev;
        core::tach::WheelConfig rc;
        rc.circumference_mm = d.settings->rear_circumference_mm;
        rc.pulses_per_rev   = d.settings->rear_pulses_per_rev;

        tach.update(c.tach_pulses, c.tach_edge_us, c.t_us, tc);
        front.update(c.front_pulses, c.front_edge_us, c.t_us, fc);
        rear.update(c.rear_pulses, c.rear_edge_us, c.t_us, rc);

        // Lamps first, before publishing anything. The rider's shift point
        // does not wait on a struct copy.
        //
        // The raw rpm drives the lamps, not the smoothed one: smoothing is
        // for a needle a human reads, and putting it in this path would add
        // lag to the one number that must not have any.
        const core::config::ShiftView sv = core::config::shift_view(*d.settings);
        core::shift::Config sc;
        sc.base_rpm = sv.base_rpm;
        sc.span_rpm = sv.span_rpm;
        sc.hysteresis_rpm = sv.hysteresis_rpm;
        sc.flash_rpm = sv.flash_rpm;

        const uint16_t rpm = static_cast<uint16_t>(tach.raw_rpm());
        const uint8_t mask = core::shift::pattern(rpm, sc, last_mask);
        if (mask != last_mask) {
            if (auto* mcp = static_cast<drivers::Mcp23s17*>(d.board)) {
                mcp->write_shift_lamps(mask);
            }
            last_mask = mask;
        }

        core::state::Engine e;
        e.rpm = tach.rpm();
        e.neutral = c.neutral;
        e.ecu_online = d.engine && d.engine->online();
        if (e.ecu_online) {
            hal::EngineData ed;
            if (d.engine->read(ed)) {
                e.coolant_c = ed.coolant_c;
                e.tps_pct = ed.tps_pct;
            }
        }
        d.slots->engine.publish(e);

        core::state::Motion m;
        m.front_mph = front.mph();
        m.rear_mph = rear.mph();
        m.gps_mph = d.slots->gnss.get().speed_mph;
        m.best_mph = core::tach::best_speed_mph(
            m.front_mph, d.health->publish().ok(core::state::Dev::McpBoard),
            m.rear_mph, true,
            m.gps_mph, d.slots->gnss.get().fix);
        d.slots->motion.publish(m);

        const uint32_t now_ms = d.clock->now_ms();
        if (hal::ok(s)) d.health->report_ok(core::state::Dev::McpBoard, now_ms);
        else            d.health->report_error(core::state::Dev::McpBoard,
                                               static_cast<int32_t>(s), now_ms);

        // Missing a deadline here is the one timing failure on this bike that
        // a rider would feel, so it is counted and shown on DIAG → System
        // rather than logged where nobody looks.
        const TickType_t before = xTaskGetTickCount();
        vTaskDelayUntil(&next, kPeriod);
        if (xTaskGetTickCount() - before > kPeriod + 1) {
            if (d.system) d.system->ctrl_late = static_cast<uint16_t>(++late);
        }
    }
}

}  // namespace

void start_ctrl(Deps& d) {
    // Core 0, above everything else on it. The BLE controller runs at 23 and
    // would outrank this, which is why §11.4 pins the radio to core 1.
    xTaskCreatePinnedToCore(ctrl_task, "ctrl", 4096, &d, 18, nullptr, 0);
    (void)kHz;
}

}  // namespace services
