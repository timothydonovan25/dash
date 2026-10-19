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

// Hands over one command, once.
class OneCommand final : public services::ICommandSource {
public:
    explicit OneCommand(core::state::CommandId id) { cmd_.id = id; }
    bool take(core::state::Command& out) override {
        if (given_) return false;
        given_ = true;
        out = cmd_;
        return true;
    }
private:
    core::state::Command cmd_;
    bool given_ = false;
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

    CASE("a receiver whose latency wanders 30-90 ms still times laps to the ms");
    {
        // Each fix arrives 30..90 ms after it was taken, a different amount
        // every time. Timed from arrival, every lap was off by the difference
        // of two of those; timed from the fixes' own GPS time
        // (core::fixtime) it is not.
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        NoCommands             none;
        deps.slots = &slots;
        deps.settings = &settings;

        const Track track;
        state.lap.set_line(at_ft(0.0, -track.r_ft), 90.0);
        constexpr uint32_t kBoot = 5000;
        uint32_t seed = 99;
        auto latency = [&](uint32_t fix) {
            seed = seed * 1103515245u + 12345u + fix;
            return 30u + (seed >> 16) % 61u;
        };
        uint32_t next_fix = 0;
        uint32_t next_rx = kBoot + latency(0);
        core::state::Gnss g;
        uint32_t laps_seen = 0;
        double worst = 0, first = 0;
        for (uint32_t t = 0; t < 6 * 35000 + 5000; t += 20) {
            const uint32_t now = kBoot + t + 3;
            while (next_rx <= now) {
                const core::geo::LatLon p = track.pos(next_fix);
                g.fix = true;
                g.lat = p.lat;
                g.lon = p.lon;
                g.speed_mph = static_cast<float>(track.mph);
                g.utc_ms = next_fix;
                g.rx_ms = next_rx;
                slots.gnss.publish(g);
                next_fix += 100;
                // Sentences come out in order: never before the last one.
                const uint32_t r = kBoot + next_fix + latency(next_fix);
                next_rx = r > next_rx ? r : next_rx + 1;
            }
            services::logic_tick(deps, state, none, now);
            const core::state::Lap lap = slots.lap.get();
            if (lap.count != laps_seen) {
                laps_seen = lap.count;
                const double e = std::fabs(static_cast<double>(lap.last_ms) - track.period_ms());
                // The first lap starts in the GPS's first second, before the
                // clock has seen a quick fix to anchor on: it is timed no
                // better than from arrival. On a ride the first crossing is
                // minutes after the fix, long after that.
                if (lap.count == 1) first = e;
                else if (e > worst) worst = e;
            }
        }
        std::printf("       (wandering latency: %u laps, first %.1f ms off, the rest at worst %.1f;"
                    " from arrival times it was 17)\n",
                    static_cast<unsigned>(laps_seen), first, worst);
        CHECK(laps_seen >= 4);
        CHECK(first <= 20.0);
        CHECK(worst <= 2.0);
    }

    CASE("splits, the ideal lap, each lap's peaks and the session's lean reach the slots");
    {
        // Six laps of the circle, leaning into it (left, anticlockwise) and
        // revving harder each lap. Fixes carry GPS time and course, as the
        // receiver's do.
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        services::LapHistory   history;
        NoCommands             none;
        deps.slots = &slots;
        deps.settings = &settings;
        deps.lap_history = &history;
        core::state::Health h;
        h[core::state::Dev::Imu].state = core::state::DevState::Ok;
        slots.health.publish(h);

        const Track track;
        state.lap.set_line(at_ft(0.0, -track.r_ft), 90.0);
        constexpr uint32_t kBoot = 5000;
        uint32_t next_fix = 0;
        bool saw_split = false, saw_flash = false;
        for (uint32_t t = 0; t < 6 * 35000 + 3000; t += 20) {
            const uint32_t now = kBoot + t + 3;
            const int lap = static_cast<int>(t / 34772);
            while (kBoot + next_fix + 40 <= now) {
                const core::geo::LatLon p = track.pos(next_fix);
                core::state::Gnss g;
                g.fix = true;
                g.lat = p.lat;
                g.lon = p.lon;
                g.speed_mph = static_cast<float>(track.mph);
                const double a = -kPi / 2 + 2.0 * kPi * next_fix / track.period_ms() - 0.05;
                double course = std::fmod(-a * 180.0 / kPi, 360.0);
                if (course < 0) course += 360.0;
                g.course_deg = static_cast<float>(course);
                g.utc_ms = 36000000u + next_fix;
                g.rx_ms = kBoot + next_fix + 40;
                slots.gnss.publish(g);
                next_fix += 100;
            }
            core::state::Motion m;
            m.best_mph = static_cast<float>(track.mph);
            slots.motion.publish(m);
            core::state::Engine e;
            e.rpm = 9000.0f + 300.0f * lap;
            slots.engine.publish(e);
            core::state::Attitude att;
            att.lean_deg = -(30.0f + lap);
            slots.attitude.publish(att);
            services::logic_tick(deps, state, none, now);
            const core::state::Lap l = slots.lap.get();
            if (l.split_ms != 0) saw_split = true;
            if (l.last_was_best && l.last_done_ms != 0) saw_flash = true;
        }
        const core::state::Lap l = slots.lap.get();
        CHECK(l.count >= 5);
        CHECK(saw_split);
        CHECK(saw_flash);
        CHECK(l.sector >= 0);
        // A steady circle: every lap the same, so the ideal is the lap.
        CHECK(l.ideal_ms > 0);
        CHECK(std::fabs(static_cast<double>(l.ideal_ms) - track.period_ms()) <= 5.0);
        CHECK(l.max_lean_left_deg >= 34.0f);
        CHECK(l.max_lean_right_deg == 0.0f);
        // Each lap's own peaks: the rpm and lean climb a step a lap.
        CHECK(history.count == l.count);
        if (history.count >= 3) {
            CHECK(history.peak[2].rpm > history.peak[1].rpm);
            CHECK(history.peak[2].lean_deg > history.peak[1].lean_deg);
            CHECK(std::fabs(history.peak[2].mph - track.mph) < 0.5);
        }

        // IMU's short E: the session's lean peaks back to zero.
        OneCommand reset(core::state::CommandId::LeanPeaksReset);
        core::state::Attitude upright;
        slots.attitude.publish(upright);
        services::logic_tick(deps, state, reset, kBoot + 6 * 35000 + 4000);
        CHECK(slots.lap.get().max_lean_left_deg == 0.0f);
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

    CASE("Set line is refused at a standstill, and says so; moving, it is set");
    {
        // The line faces the GPS course, which means nothing stopped: a
        // rider who turned round at the line would get one no lap crosses.
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        deps.slots = &slots;
        deps.settings = &settings;

        core::state::Gnss g;
        g.fix = true;
        g.hdop = 1.0f;
        g.lat = kLat0;
        g.lon = kLon0;
        g.course_deg = 270.0f;
        g.speed_mph = 1.5f;                          // parked, Doppler near 0
        g.rx_ms = 1000;
        slots.gnss.publish(g);
        OneCommand stopped(core::state::CommandId::LapSetLine);
        services::logic_tick(deps, state, stopped, 1000);
        CHECK(!state.lap.has_line());
        CHECK(slots.lap.get().line_refused_seq == 1);

        g.speed_mph = 25.0f;
        g.rx_ms = 1100;
        slots.gnss.publish(g);
        OneCommand moving(core::state::CommandId::LapSetLine);
        services::logic_tick(deps, state, moving, 1100);
        CHECK(state.lap.has_line());
        CHECK(slots.lap.get().line_refused_seq == 1);  // not refused again
    }

    CASE("Calibrate level is handed to imu, and its answer applied like any edit");
    {
        core::state::Slots     slots;
        core::config::Settings settings;
        services::Deps         deps;
        services::LogicState   state;
        deps.slots = &slots;
        deps.settings = &settings;

        const uint32_t gen = slots.imu_request.generation();
        OneCommand cal(core::state::CommandId::ImuCalibrate);
        services::logic_tick(deps, state, cal, 1000);
        CHECK(slots.imu_request.generation() != gen);
        CHECK(slots.imu_request.get().op == core::state::ImuRequest::CalibrateLevel);

        // imu's answer: unrounded, and past the old ±45° pitch range.
        class Two final : public services::ICommandSource {
        public:
            bool take(core::state::Command& out) override {
                if (n_ == 0) out = core::state::Command::config_set(core::config::kKeyImuRollOffset, 3.27f);
                else if (n_ == 1) out = core::state::Command::config_set(core::config::kKeyImuPitchOffset, 52.4f);
                else return false;
                ++n_;
                return true;
            }
        private:
            int n_ = 0;
        } answer;
        services::logic_tick(deps, state, answer, 1020);
        CHECK_NEAR(settings.imu_roll_offset_deg, 3.27, 1e-5);
        CHECK_NEAR(settings.imu_pitch_offset_deg, 52.4, 1e-5);
    }

    return check::report("logic_lap");
}
