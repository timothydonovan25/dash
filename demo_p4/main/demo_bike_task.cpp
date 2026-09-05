#include "demo_bike_task.hpp"
#include "sim_bike.hpp"
#include "services/logic_tick.hpp"
#include "core/shift/shift.hpp"
#include "core/gear/gear.hpp"
#include "core/config/config.hpp"
#include "ble_media.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

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

// The circuit (sim/sim_bike.cpp's kLap_ft) is ~3057 ft. A lap only counts
// on the *second* crossing of the line after arming — the first just
// starts the clock (core::lap::Phase::Armed -> Timing) — and delta needs a
// second completed lap to have a session best to compare against, so a
// visitor has to watch three crossings before anything shows on the delta
// screen. Averaging 50 mph (73.3 ft/s; the sinusoid's average is its
// midpoint regardless of amplitude) puts that at 3 * 3057/73.3 ≈ 125s —
// under the two and a half minutes it's reasonable to expect someone to
// stand there, unlike this function's first version, which averaged 35mph
// and needed over three minutes for the same three crossings.
void animate(sim::Controls& c, uint32_t t_ms) {
    const float phase = static_cast<float>(t_ms) / 1000.0f;
    const float speed = 50.0f + 20.0f * std::sin(phase * 0.25f);
    c.speed_mph = speed;
    c.rpm       = 2500.0f + 55.0f * c.speed_mph;
    c.tps_pct   = 30.0f + 25.0f * std::sin(phase * 0.25f + 0.5f);
    c.auto_ride = true;
    c.gps_fix   = true;
}

void bike_task(void* arg) {
    auto* pair = static_cast<std::pair<services::Deps*, services::CommandQueue*>*>(arg);
    services::Deps& d = *pair->first;
    services::CommandQueue& q = *pair->second;

    sim::Bike bike;
    bike.reset();
    sim::Controls controls;   // media_* fields stay empty — see below

    services::LogicState logic;
    services::logic_restore(d, logic);
    QueueCommandSource commands(q);

    uint8_t lamp_mask = 0;
    uint32_t now = d.clock->now_ms();
    bool lap_armed = false;
    // generation(), not the value itself: media_ctrl has no way to tell "a
    // new one arrived" from "still the last one" other than this, since it
    // is deliberately not a queue (see core::state::MediaCtrl).
    uint32_t ctrl_gen = 0;

    for (;;) {
        now += kTickMs;
        animate(controls, now);
        bike.step(*d.slots, controls, now, kTickMs);
        // sim::Bike publishes Media itself from Controls.media_* (the
        // desktop sim's only way to fake a phone), which would otherwise
        // fight the real one here every tick. controls.media_title is left
        // empty above so that publish is always a no-op clear, and this is
        // the actual, single write — ble_media.cpp's own callback never
        // touches the slot directly, only this cache (rule 2: one writer).
        d.slots->media.publish(demo::ble_media_snapshot());
        d.slots->ble_diag.publish(demo::ble_media_diag());

        // Set once the bike has a fix to set it against (auto_ride/gps_fix
        // are always on here, so this fires on the very first tick), then
        // armed: the timer scores a lap every time the bike passes this
        // point on the circuit, and once there's a session best, the delta
        // screen has something to compare the current lap against.
        if (!lap_armed && d.slots->gnss.get().fix) {
            q.post(core::state::Command(core::state::CommandId::LapSetLine));
            q.post(core::state::Command(core::state::CommandId::LapArm));
            lap_armed = true;
        }

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

        const core::config::ShiftView sv = core::config::shift_view(*d.settings);
        core::shift::Config sc;
        sc.base_rpm       = sv.base_rpm;
        sc.span_rpm       = sv.span_rpm;
        sc.hysteresis_rpm = sv.hysteresis_rpm;
        sc.flash_rpm      = sv.flash_rpm;
        const uint16_t rpm = static_cast<uint16_t>(controls.rpm);
        lamp_mask = core::shift::pattern(rpm, sc, lamp_mask);
        controls.shift_mask  = lamp_mask;
        controls.shift_flash = core::shift::over_limit(rpm, sc);
        controls.gear = core::gear::estimate(controls.rpm, controls.speed_mph,
                                             controls.neutral,
                                             core::config::gear_view(*d.settings));

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
