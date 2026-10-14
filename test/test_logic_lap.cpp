// `logic`'s half of the lap timer: it runs at 50 Hz and the receiver at
// 10 Hz, and a lap must still be timed from the fixes, not the ticks.
// core::lap itself is test_lap.
#include "check.hpp"
#include "services/logic_tick.hpp"
#include <cmath>

namespace {

class NoCommands final : public services::ICommandSource {
public:
    bool take(core::state::Command&) override { return false; }
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kLat0 = 40.0;
constexpr double kLon0 = -105.0;
constexpr double kFtPerDegLat = 364000.0;

// Local feet (x east, y north) to a fix near the origin.
core::geo::LatLon at_ft(double x, double y) {
    return {kLat0 + y / kFtPerDegLat,
            kLon0 + x / (kFtPerDegLat * std::cos(kLat0 * kPi / 180.0))};
}

// A circle of radius r ridden anticlockwise at a steady speed, the line
// across its south point, crossed eastbound.
struct Track {
    double r_ft = 487.0;   // a period that is not a whole number of fixes, so
                           // each crossing lands at a different phase
    double mph = 60.0;
    double period_ms() const { return 2.0 * kPi * r_ft / (mph * 5280.0 / 3600.0) * 1000.0; }
    core::geo::LatLon pos(double t_ms) const {
        const double a = -kPi / 2 + 2.0 * kPi * t_ms / period_ms() - 0.05;
        return at_ft(r_ft * std::cos(a), r_ft * std::sin(a));
    }
};

}  // namespace

int main() {
    CASE("laps are timed from the fixes, not from the 50 Hz ticks");
    {
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        NoCommands             none;
        deps.slots = &slots;
        deps.settings = &settings;

        const Track track;
        state.lap.set_line(at_ft(0.0, -track.r_ft), 90.0);

        // Fixes every 100 ms, stamped when they would arrive: a fixed
        // receiver latency plus up to 4 ms of poll jitter. logic ticks every
        // 20 ms, out of phase with the receiver, and sees whichever fix is
        // newest.
        constexpr uint32_t kBoot = 5000;
        constexpr uint32_t kLatencyMs = 37;
        uint32_t next_fix = 0;
        core::state::Gnss g;
        uint32_t current_before = 0;
        int advanced_between_fixes = 0;
        for (uint32_t t = 0; t < 4 * 35000 + 5000; t += 20) {
            const uint32_t now = kBoot + t + 3;
            bool fresh = false;
            while (kBoot + next_fix + kLatencyMs + (next_fix / 100 * 7) % 5 <= now) {
                const core::geo::LatLon p = track.pos(next_fix);
                g.fix = true;
                g.lat = p.lat;
                g.lon = p.lon;
                g.speed_mph = static_cast<float>(track.mph);
                g.utc_ms = next_fix;
                g.rx_ms = kBoot + next_fix + kLatencyMs + (next_fix / 100 * 7) % 5;
                slots.gnss.publish(g);
                next_fix += 100;
                fresh = true;
            }
            services::logic_tick(deps, state, none, now);
            const uint32_t cur = slots.lap.get().current_ms;
            if (!fresh && cur > current_before) ++advanced_between_fixes;
            current_before = cur;
        }

        const core::state::Lap lap = slots.lap.get();
        std::printf("       (laps %u, last %u ms, expected %.1f ms)\n",
                    static_cast<unsigned>(lap.count),
                    static_cast<unsigned>(lap.last_ms), track.period_ms());
        CHECK(lap.count >= 2);
        // Within the receiver's poll jitter and the chord-versus-arc error
        // of a straight line between fixes. Interpolating over the tick
        // interval instead put it tens of milliseconds out.
        CHECK(std::fabs(static_cast<double>(lap.last_ms) - track.period_ms()) <= 6.0);
        CHECK(std::fabs(static_cast<double>(lap.best_ms) - track.period_ms()) <= 6.0);
        // The running clock still counts at the tick rate between fixes.
        CHECK(advanced_between_fixes > 1000);
    }

    CASE("a receiver that goes silent is told to the timer as no fix");
    {
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        NoCommands             none;
        deps.slots = &slots;
        deps.settings = &settings;

        const Track track;
        state.lap.set_line(at_ft(0.0, -track.r_ft), 90.0);
        uint32_t now = 1000;
        uint32_t fix_t = 0;
        // Ride over the line so the lap starts, then the receiver stops.
        for (; fix_t < 4000; fix_t += 100, now += 100) {
            core::state::Gnss g;
            const core::geo::LatLon p = track.pos(fix_t);
            g.fix = true;
            g.lat = p.lat;
            g.lon = p.lon;
            g.speed_mph = static_cast<float>(track.mph);
            g.rx_ms = now;
            slots.gnss.publish(g);
            services::logic_tick(deps, state, none, now);
        }
        CHECK(slots.lap.get().phase == core::lap::Phase::Timing);
        for (int i = 0; i < 100; ++i) {                 // 2 s of silence
            now += 20;
            services::logic_tick(deps, state, none, now);
        }
        // Resumes after the gap: the timer must have seen the dropout, so
        // the lap that spans it cannot be a clean one.
        for (; fix_t < 40000; fix_t += 100, now += 100) {
            core::state::Gnss g;
            const core::geo::LatLon p = track.pos(fix_t);
            g.fix = true;
            g.lat = p.lat;
            g.lon = p.lon;
            g.speed_mph = static_cast<float>(track.mph);
            g.rx_ms = now;
            slots.gnss.publish(g);
            services::logic_tick(deps, state, none, now);
        }
        CHECK(slots.lap.get().count == 1);
        CHECK(slots.lap.get().best_ms == 0);            // gapped: not a best
    }

    return check::report("logic_lap");
}
