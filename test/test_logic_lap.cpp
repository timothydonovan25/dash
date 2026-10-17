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

    CASE("every fix makes a trace point, whatever the tick phase");
    {
        // Fixes 100 ms apart, stamped with the gnss task's 5 ms poll jitter
        // (alternately 95 and 105 ms apart), and drifting against the 20 ms
        // logic tick the way two free-running clocks do. Where the phase is
        // wrong, the ticks that see two fixes are 80 ms apart, and a gate
        // on the tick time dropped the second of them.
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        NoCommands             none;
        auto* trace = new core::state::TrackBuffer<core::state::TrackPoint, 4096>();
        deps.slots = &slots;
        deps.settings = &settings;
        deps.trip_trace = trace;

        const Track track;
        core::state::Gnss g;
        uint32_t next_fix = 0;
        int fixes = 0;
        for (uint32_t now = 10000; now < 20000; now += 20) {
            const uint32_t k = next_fix / 100;
            const uint32_t arrive = 10000 + next_fix + (k % 2) * 5 + (k * 3) / 10;
            if (arrive <= now) {
                const core::geo::LatLon p = track.pos(next_fix);
                g.fix = true;
                g.lat = p.lat;
                g.lon = p.lon;
                g.speed_mph = static_cast<float>(track.mph);
                g.utc_ms = next_fix;
                g.rx_ms = arrive;
                slots.gnss.publish(g);
                next_fix += 100;
                ++fixes;
            }
            services::logic_tick(deps, state, none, now);
        }
        CHECK(fixes > 90);
        CHECK(trace->size() + 1 >= static_cast<size_t>(fixes));
        delete trace;
    }

    CASE("a fix is acted on in the pass it wakes; blackbox rows keep the tick grid");
    {
        // The firmware's `logic`: Scheduled ticks every 20 ms, and an Event
        // pass when the gnss task publishes a fix, out of phase with them.
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        NoCommands             none;
        auto* trace = new core::state::TrackBuffer<core::state::TrackPoint, 4096>();
        auto* box = new core::log::Ring<core::log::Record, 512>();
        settings.log_rate = static_cast<uint8_t>(core::log::Rate::Hz10);
        deps.slots = &slots;
        deps.settings = &settings;
        deps.trip_trace = trace;
        deps.blackbox = box;

        const Track track;
        core::state::Gnss g;
        int ticks = 0;
        int rows = 0;
        int off_grid = 0;
        int traced_late = 0;
        constexpr uint32_t kStart = 10000;
        for (uint32_t t = 0; t < 10000; t += 20) {
            const uint32_t now = kStart + t;
            services::logic_tick(deps, state, none, now,
                                 services::LogicPass::Scheduled);
            ++ticks;
            // A fix landing 2 ms before every 10 Hz row is due, woken at
            // once: inside the decimator's slack, so an Event pass allowed
            // to log would take the row and pull the grid onto the fixes.
            if (t % 100 == 80) {
                const uint32_t arrive = now + 18;
                const core::geo::LatLon p = track.pos(t);
                g.fix = true;
                g.lat = p.lat;
                g.lon = p.lon;
                g.speed_mph = static_cast<float>(track.mph);
                g.utc_ms = t;
                g.rx_ms = arrive;
                slots.gnss.publish(g);
                const size_t before = trace->size();
                services::logic_tick(deps, state, none, arrive,
                                     services::LogicPass::Event);
                // In the wake's own pass, not at the next tick.
                if (t > 100 && trace->size() != before + 1) ++traced_late;
            }
            core::log::Record r;
            while (box->pop(r)) {
                ++rows;
                if ((r.t_ms - kStart) % 100 != 0 && r.lap_done_ms == 0) ++off_grid;
            }
        }
        CHECK(traced_late == 0);
        CHECK(off_grid == 0);
        // One row per fifth tick at 10 Hz, give or take a lap tag.
        CHECK(rows >= ticks / 5 - 1 && rows <= ticks / 5 + 2);
        delete box;
        delete trace;
    }

    return check::report("logic_lap");
}
