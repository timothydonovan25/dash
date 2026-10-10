// `logic`'s half of the gear calibration: commands in, Settings and the
// GearCal slot out. The state machines themselves are test_gearcal.
#include "check.hpp"
#include "services/logic_tick.hpp"
#include <deque>

namespace {

class Queue final : public services::ICommandSource {
public:
    bool take(core::state::Command& out) override {
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    void post(core::state::CommandId id) { q.emplace_back(id); }
    std::deque<core::state::Command> q;
};

struct Rig {
    core::state::Slots     slots;
    core::config::Settings settings;
    services::Deps         deps;
    services::LogicState   state;
    Queue                  queue;
    uint32_t               now = 1000;

    Rig() {
        deps.slots = &slots;
        deps.settings = &settings;
    }

    // Ride at `rpm` and `mph` (GPS agreeing) for `ms`.
    void ride(float rpm, float mph, uint32_t ms, bool neutral = false) {
        for (uint32_t i = 0; i < ms; i += 20) {
            core::state::Engine e;
            e.rpm = rpm * (1.0f + (static_cast<int>(now / 20 % 5) - 2) * 0.001f);
            e.neutral = neutral;
            slots.engine.publish(e);
            core::state::Motion m;
            m.best_mph = mph;
            m.front_mph = m.rear_mph = m.gps_mph = mph;
            slots.motion.publish(m);
            core::state::Gnss g;
            g.fix = true;
            g.hdop = 1.0f;
            g.speed_mph = mph;
            g.utc_ms = now;
            slots.gnss.publish(g);
            services::logic_tick(deps, state, queue, now);
            now += 20;
        }
    }
};

}  // namespace

int main() {
    using core::state::CommandId;
    using core::gearcal::Phase;

    CASE("Calibrate gears opens the page; cancel closes it and changes nothing");
    {
        Rig r;
        const float before = r.settings.gear_rpm_per_mph[0];
        r.queue.post(CommandId::GearCalStart);
        r.ride(6000, 14, 40);
        CHECK(r.slots.gear_cal.get().phase == static_cast<uint8_t>(Phase::Intro));
        CHECK(r.slots.gear_cal.get().count == 5);
        r.queue.post(CommandId::GearCalCancel);
        r.ride(6000, 14, 40);
        CHECK(r.slots.gear_cal.get().phase == 0);
        CHECK_NEAR(r.settings.gear_rpm_per_mph[0], before, 1e-6);
    }

    CASE("a run accepted writes the table and the learner's base together");
    {
        Rig r;
        r.queue.post(CommandId::GearCalStart);
        r.queue.post(CommandId::GearCalNext);
        // 1st, 2 % high: 420 * 1.02 = 428.4 rpm per mph.
        r.ride(6000, 6000.0f / 428.4f, 11000);
        CHECK(r.slots.gear_cal.get().ready);
        CHECK(r.slots.gear_cal.get().gear == 1);
        CHECK_NEAR(r.slots.gear_cal.get().live_err_pct, 2.0, 0.8);
        r.queue.post(CommandId::GearCalNext);       // keep 1st, on to 2nd
        r.ride(6000, 6000.0f / 428.4f, 40);
        CHECK(r.slots.gear_cal.get().gear == 2);
        for (int g = 2; g <= 5; ++g) r.queue.post(CommandId::GearCalSkip);
        r.ride(6000, 12, 40);
        CHECK(r.slots.gear_cal.get().phase == static_cast<uint8_t>(Phase::Summary));
        CHECK(r.slots.gear_cal.get().done_mask == 0x01);
        // Nothing is written until it is accepted.
        CHECK_NEAR(r.settings.gear_rpm_per_mph[0], 420.0, 1e-6);
        r.queue.post(CommandId::GearCalNext);
        r.ride(6000, 12, 40);
        CHECK(r.slots.gear_cal.get().phase == 0);
        CHECK_NEAR(r.settings.gear_rpm_per_mph[0], 428.4, 0.6);
        CHECK_NEAR(r.settings.gear_base_rpm_per_mph[0], r.settings.gear_rpm_per_mph[0], 1e-6);
        CHECK_NEAR(r.settings.gear_rpm_per_mph[1], 290.0, 1e-6);   // skipped
    }

    CASE("typing a ratio in CONFIG makes it the new base");
    {
        Rig r;
        r.settings.gear_rpm_per_mph[2] = 205.0f;
        r.queue.q.push_back(core::state::Command::config_set(0x0804, 205.0f));
        r.ride(3000, 4, 40);
        CHECK_NEAR(r.settings.gear_base_rpm_per_mph[2], 205.0, 1e-6);
    }

    CASE("Reset to base puts the table back");
    {
        Rig r;
        r.settings.gear_rpm_per_mph[3] = 170.0f;     // learned away from 165
        r.queue.post(CommandId::GearCalReset);
        r.ride(3000, 4, 40);
        CHECK_NEAR(r.settings.gear_rpm_per_mph[3], 165.0, 1e-6);
    }

    CASE("auto-adjust off leaves the learner out of it");
    {
        Rig r;
        r.settings.gear_autotune = false;
        for (int i = 0; i < 4; ++i) {
            r.ride(6000, 6000.0f / (215.0f * 1.05f), 7000);
            r.ride(6000, 12, 31000, true);
        }
        CHECK(r.slots.gear_cal.get().learn_n[2] == 0);
    }

    CASE("auto-adjust on notes the error and, after eight agreeing episodes, moves 1 %");
    {
        Rig r;
        for (int i = 0; i < 8; ++i) {
            r.ride(6000, 6000.0f / (215.0f * 1.05f), 7000);
            r.ride(6000, 12, 31000, true);
            if (i == 3) {
                CHECK(r.slots.gear_cal.get().learn_n[2] == 4);
                CHECK_NEAR(r.slots.gear_cal.get().learn_err_pct[2], 5.0, 1.2);
                CHECK_NEAR(r.settings.gear_rpm_per_mph[2], 215.0, 1e-6);
            }
        }
        CHECK_NEAR(r.settings.gear_rpm_per_mph[2], 217.2, 0.2);
        CHECK(r.slots.gear_cal.get().adjusted_mask == 0x04);
        // The base is where learning started, and does not follow it.
        CHECK_NEAR(r.settings.gear_base_rpm_per_mph[2], 215.0, 1e-6);
    }

    return check::report("logic_gearcal");
}
