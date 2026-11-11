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
    CASE("the detail log's row shows a fix that arrived on an Event pass");
    {
        // Fixes wake logic between its 50 Hz ticks; the row is written only
        // on a tick, and showed GpsNew 0 and GPS never used for whole rides.
        Bike b;
        b.settings.detail_log = true;
        static core::log::DetRing det;
        b.deps.det_log = &det;
        int rows = 0, with_gps = 0;
        for (uint32_t t = 1000; t < 3000; t += 20) {
            core::state::Motion m;
            m.front_mph = m.rear_mph = m.best_mph = 20.0f;
            b.slots.motion.publish(m);
            services::logic_tick(b.deps, b.s, b.q, t);
            if (t % 100 == 0) {
                core::state::Gnss g;
                g.fix = true;
                g.hdop = 1.0f;
                g.speed_mph = 20.0f;
                g.utc_ms = 36000000u + t;
                g.rx_ms = t + 7;
                b.slots.gnss.publish(g);
                services::logic_tick(b.deps, b.s, b.q, t + 7, services::LogicPass::Event);
            }
        }
        core::log::DetRow r;
        while (det.pop(r)) {
            ++rows;
            if (r.gps_new && (r.used & core::fusion::kSrcGps)) ++with_gps;
        }
        CHECK(rows >= 95 && rows <= 100);        // still one a tick, no more
        CHECK(with_gps >= 18);                   // a fix every 100 ms
    }

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

    CASE("demo mode: a standing start times nothing");
    {
        for (bool demo : {false, true}) {
            Bike b;
            b.settings.demo_mode = demo;
            b.run(3000, 0.0f, false, 1500.0f, 0);              // stopped: armed
            for (float mph = 1.0f; mph <= 40.0f; mph += 1.0f)  // ~8 mph/s
                b.run(120, mph, false, 6000.0f, 0);
            CHECK((b.slots.perf.get().seq != 0) == !demo);
        }
    }

    return check::report("logic_hr");
}
