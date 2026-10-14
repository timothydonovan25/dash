// `ctrl` — core 0, priority 18, 500 Hz.
//
// The only latency-critical task on the bike. Tach edge to shift lamp has to
// stay under about 3 ms: one 500 Hz tick is 2 ms worst case, and one I2C
// write to the LED driver is about 0.1 ms at 1 MHz. Everything else here is
// bookkeeping that happens to share the loop.
//
// Nothing in this file is allowed to block, allocate, touch the filesystem
// or log. If a line here can wait, it belongs in `logic`.
#include "services/services.hpp"
#include "core/shift/shift.hpp"
#include "core/gear/gear.hpp"
#include "core/tach/tach.hpp"
#include "core/power/power.hpp"
#include "core/sweep/sweep.hpp"
#include "bsp/pins.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr uint32_t kHz = 500;
constexpr TickType_t kPeriod = pdMS_TO_TICKS(2);

// The LED driver comes out of a brown-out with every duty at zero and
// acknowledges as if nothing happened. The mask is only written when it
// changes, so without this a steady bar would stay dark until the rpm moved.
// Once a second is 0.1 ms of I2C per 500 ticks.
constexpr uint32_t kResendTicks = kHz;

// After a failed lamp write, wait this long before the next attempt rather
// than retrying every tick: a wedged bus would otherwise spend each tick in
// an I2C timeout (rule 5).
constexpr uint32_t kRetryTicks = kHz / 20;   // 50 ms

void ctrl_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::tach::Meter      tach;
    core::tach::WheelMeter front;
    core::tach::WheelMeter rear;
    // `prev_mask` is what the pattern's hysteresis compares against, and
    // moves every tick; `lit_mask` is what the LED driver has acknowledged.
    // They differ only after a failed write, which is retried, not forgotten.
    uint8_t prev_mask = 0;
    uint8_t lit_mask = 0;
    uint8_t last_master = 0xFF;      // nothing valid yet, so the first pass writes
    uint32_t ticks = 0;
    uint32_t retry_tick = 0;
    // The start-up sweep (core/sweep): the lamps follow the screen's tach up
    // and back, from render's first frame. If no frame has gone out by
    // kSweepWaitMs -- a dead panel -- there is no tach to follow, so skip it.
    bool sweep_done = false;
    constexpr uint32_t kSweepWaitMs = 3000;

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
        uint16_t lamp_rpm = rpm;
        if (!sweep_done) {
            const uint32_t now = d.clock->now_ms();
            const uint32_t first = d.system ? d.system->first_frame_ms : 0;
            if (first != 0) {
                core::sweep::Config wc;
                wc.enabled = d.settings->gauge_sweep;
                wc.duration_ms = d.settings->sweep_ms;
                wc.peak_rpm = d.settings->shift_flash_rpm;
                const float road = front.mph() > rear.mph() ? front.mph() : rear.mph();
                const float sw = core::sweep::lamp_rpm(now - first, road, wc);
                if (sw < 0.0f) sweep_done = true;
                else if (sw > rpm) lamp_rpm = static_cast<uint16_t>(sw);
            } else if (now > kSweepWaitMs || !d.settings->gauge_sweep) {
                sweep_done = true;
            }
        }
        const uint8_t mask = core::shift::pattern(lamp_rpm, sc, prev_mask);
        prev_mask = mask;
        const bool resend = ++ticks % kResendTicks == 0;
        const bool may_try = static_cast<int32_t>(ticks - retry_tick) >= 0;
        hal::Status lamp_status = hal::Status::Ok;
        bool wrote = false;
        if (((mask != lit_mask && may_try) || resend) && d.lamps) {
            lamp_status = d.lamps->set_mask(mask);
            if (hal::ok(lamp_status)) lit_mask = mask;
            else retry_tick = ticks + kRetryTicks;
            wrote = true;
        }

        // The master duty is the brightness control. On USB the whole board
        // shares a 500 mA port, so it is capped there. The VIN sense says
        // which supply it is; until it has a reading, a turning engine is
        // the fallback evidence that this is the bike, as it was on the S3
        // board, which had no VIN sense at all.
        const core::power::Source src =
            core::power::classify(d.slots->elec.get().vbat);
        const bool full = src == core::power::Source::Bike ||
                          (src == core::power::Source::Unknown && tach.running());
        // Dark while the supply is failing: the lamps are the biggest load
        // on the rail that is still holding the board up (see `power`).
        const uint8_t master = d.power_fail.load(std::memory_order_relaxed)
            ? 0
            : core::power::lamp_master_pct(
                  full, d.settings->shift_brightness_pct,
                  static_cast<uint8_t>(bsp::kShiftDutyUsbMax * 100 / 255));
        if (master != last_master && may_try && d.lamps) {
            const hal::Status ms = d.lamps->set_master(master);
            if (hal::ok(ms)) last_master = master;
            else retry_tick = ticks + kRetryTicks;
            if (hal::ok(lamp_status)) lamp_status = ms;
            wrote = true;
        }

        const uint32_t now_ms_early = d.clock->now_ms();

        // A wheel sensor `logic` has found dead (read 0 at speed) is skipped,
        // so the speedo falls through to the other wheel, then GPS, instead
        // of showing 0 mph for the rest of the ride.
        const core::state::Health hl = d.slots->health.get();
        const bool front_live = hl[core::state::Dev::FrontWheel].state != core::state::DevState::Fault;
        const bool rear_live = hl[core::state::Dev::RearWheel].state != core::state::DevState::Fault;
        const core::state::Gnss g = d.slots->gnss.get();

        core::state::Motion m;
        m.front_mph = front.mph();
        m.rear_mph = rear.mph();
        m.gps_mph = g.speed_mph;
        m.best_mph = core::tach::best_speed_mph(
            m.front_mph, hal::ok(s) && front_live,
            m.rear_mph, hal::ok(s) && rear_live,
            m.gps_mph, g.fix);

        core::state::Engine e;
        e.shift_mask = mask;
        e.shift_flash = core::shift::over_limit(lamp_rpm, sc);
        e.rpm = tach.rpm();
        e.neutral = c.neutral;
        // Needs both rpm and road speed, and this is the one task that has
        // them first-hand. Before the publish: it was once set after it, on
        // a copy nobody read again, and the gear read 0 on every screen.
        e.gear = core::gear::estimate(tach.rpm(), m.best_mph, c.neutral,
                                      core::config::gear_view(*d.settings));
        e.ecu_online = d.engine && d.engine->online();
        if (e.ecu_online) {
            hal::EngineData ed;
            if (d.engine->read(ed)) {
                e.coolant_c = ed.coolant_c;
                e.tps_pct = ed.tps_pct;
            }
        }
        d.slots->engine.publish(e);
        d.slots->motion.publish(m);

        // Only a write says anything about the LED driver. A quiet tick is
        // not evidence either way, and the resend makes sure there is at
        // least one write a second for the tracker to see.
        const uint32_t now_ms = now_ms_early;
        if (wrote) {
            if (hal::ok(lamp_status)) {
                d.health->report_ok(core::state::Dev::LedDriver, now_ms);
            } else {
                d.health->report_error(core::state::Dev::LedDriver,
                                       static_cast<int32_t>(lamp_status), now_ms);
            }
        }

        // Missing a deadline here is the one timing failure on this bike that
        // a rider would feel, so it is counted and shown on DIAG → System
        // rather than logged where nobody looks.
        //
        // Lateness against the schedule, not the length of the sleep: a tick
        // that overran (a stuck I2C write, a flash write freezing the core)
        // makes vTaskDelayUntil return at once, and timing the sleep counted
        // that as zero. After an overrun the schedule restarts from now
        // instead of running the missed ticks back to back.
        vTaskDelayUntil(&next, kPeriod);
        const TickType_t woke = xTaskGetTickCount();
        if (woke - next > 1) {
            if (d.system) d.system->ctrl_late = static_cast<uint16_t>(++late);
            next = woke;
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
