#include "demo_bike_task.hpp"
#include "sim_bike.hpp"
#include "services/logic_tick.hpp"
#include "core/shift/shift.hpp"
#include "core/gear/gear.hpp"
#include "core/config/config.hpp"

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

// A lap in about 40 seconds at the peak speed below, so a visitor watching
// the demo sees a handful of laps score during a normal conversation.
void animate(sim::Controls& c, uint32_t t_ms) {
    const float phase = static_cast<float>(t_ms) / 1000.0f;
    const float speed = 35.0f + 30.0f * std::sin(phase * 0.25f);
    c.speed_mph = speed < 3.0f ? 3.0f : speed;
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
    sim::Controls controls;

    services::LogicState logic;
    services::logic_restore(d, logic);
    QueueCommandSource commands(q);

    uint8_t lamp_mask = 0;
    uint32_t now = d.clock->now_ms();

    for (;;) {
        now += kTickMs;
        animate(controls, now);
        bike.step(*d.slots, controls, now, kTickMs);

        services::logic_tick(d, logic, commands, now);

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
