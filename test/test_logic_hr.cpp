// `logic`'s half of heart-rate recovery: the speed it fuses, the engine and
// the watch's slot in, the hr_recovery slot out. The rules are test_hr.
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
    std::deque<core::state::Command> q;
};

struct Bike {
    core::state::Slots slots;
    core::config::Settings settings;
    services::Deps deps;
    services::LogicState s;
    Queue q;
    uint32_t t = 1000;

    Bike() {
        deps.slots = &slots;
        deps.settings = &settings;
    }
    // `ms` of 20 ms passes at `mph`, the wheels and a 10 Hz GPS agreeing.
    void run(uint32_t ms, float mph, bool neutral, float rpm, uint16_t bpm) {
        for (uint32_t end = t + ms; t < end; t += 20) {
            core::state::Motion m;
            m.front_mph = m.rear_mph = m.best_mph = mph;
            slots.motion.publish(m);
            if (t % 100 == 0) {
                core::state::Gnss g;
                g.fix = true;
                g.hdop = 1.0f;
                g.speed_mph = mph;
                g.utc_ms = 36000000u + t;
                g.rx_ms = t;
                slots.gnss.publish(g);
            }
            core::state::Engine e;
            e.neutral = neutral;
            e.rpm = rpm;
            slots.engine.publish(e);
            core::state::Heart h;
            h.link = core::state::Heart::Live;
            h.bpm = bpm;
            h.at_ms = t;
            slots.heart.publish(h);
            services::logic_tick(deps, s, q, t);
        }
    }
};

}  // namespace

int main() {
    CASE("a ride, then parked in neutral: logic publishes the minute's fall");
    {
        Bike b;
        b.run(150000, 30.0f, false, 6000.0f, 160);       // two and a half minutes
        b.run(3000, 0.0f, false, 1500.0f, 158);          // stopped, finding neutral
        b.run(20000, 0.0f, true, 1500.0f, 150);
        CHECK(b.slots.hr_recovery.get().measuring);
        CHECK(b.slots.hr_recovery.get().count == 0);
        b.run(40000, 0.0f, true, 1500.0f, 128);
        const core::state::HrRecovery r = b.slots.hr_recovery.get();
        CHECK(r.count == 1);
        CHECK(r.from_bpm >= 158 && r.from_bpm <= 160);
        CHECK(r.drop1 >= 30 && r.drop1 <= 32);
    }

    CASE("stopped in gear with it running, as in a technical section: nothing");
    {
        Bike b;
        b.run(150000, 30.0f, false, 6000.0f, 160);
        b.run(70000, 0.0f, false, 1800.0f, 150);
        CHECK(!b.slots.hr_recovery.get().measuring);
        CHECK(b.slots.hr_recovery.get().count == 0);
    }

    return check::report("logic_hr");
}
