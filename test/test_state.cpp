#include "check.hpp"
#include "core/state/slots.hpp"
#include "core/state/track_buffer.hpp"
#include "core/state/command.hpp"

#include <thread>
#include <atomic>

using namespace core::state;

int main() {
    CASE("a frame is one coherent read of every slot");
    {
        Slots s;
        Engine e; e.rpm = 7200.0f; e.neutral = true;
        Gnss   g; g.fix = true; g.sats = 11; g.speed_mph = 54.0f;
        s.engine.publish(e);
        s.gnss.publish(g);

        const Frame f = snapshot(s, 12345);
        CHECK_NEAR(f.engine.rpm, 7200.0, 1e-3);
        CHECK(f.engine.neutral);
        CHECK(f.gnss.fix);
        CHECK(f.gnss.sats == 11);
        CHECK(f.t_ms == 12345);
        // Never-published slots read as their defaults, not as garbage.
        CHECK_NEAR(f.trip.odo_mi, 0.0, 1e-9);
        CHECK(f.lap.phase == core::lap::Phase::NoLine);
    }

    CASE("health is a table the diag screen can walk");
    {
        Health h;
        CHECK(!h.ok(Dev::Imu));                     // Absent until proven Ok
        h[Dev::Imu].state = DevState::Ok;
        h[Dev::Gnss].state = DevState::Fault;
        h[Dev::Gnss].fault_count = 3;
        CHECK(h.ok(Dev::Imu));
        CHECK(!h.ok(Dev::Gnss));
        CHECK(h[Dev::Gnss].fault_count == 3);
        for (uint8_t i = 0; i < kDevCount; ++i) {
            CHECK(dev_name(static_cast<Dev>(i))[0] != '?');
        }
    }

    CASE("the lap slot carries the timer state plus a delta it does not own");
    {
        core::lap::State ls;
        ls.phase = core::lap::Phase::Timing;
        ls.count = 4;
        ls.best_ms = 91250;
        const Lap p = to_published(ls, -320);
        CHECK(p.count == 4);
        CHECK(p.best_ms == 91250);
        CHECK(p.delta_ms == -320);
    }

    CASE("commands carry their argument in the right union arm");
    {
        const Command a(CommandId::TripReset, int32_t{1});
        CHECK(a.i == 1);
        const Command b = Command::config_set(0x0042, 21.5f);
        CHECK(b.id == CommandId::ConfigSet);
        CHECK(b.cfg.key == 0x0042);
        CHECK_NEAR(b.cfg.val, 21.5, 1e-6);
        CHECK(std::string(command_name(CommandId::LapSetLine)) == "lap.set_line");
    }

    CASE("track buffer reads back what was written");
    {
        TrackBuffer<TrackPoint, 8> tb;
        CHECK(tb.size() == 0);
        CHECK(!tb.full());
        for (int i = 0; i < 5; ++i) {
            TrackPoint p;
            p.x_ft = static_cast<float>(i);
            p.t_ms = static_cast<uint32_t>(i * 100);
            tb.push(p);
        }
        CHECK(tb.size() == 5);
        CHECK(tb.total() == 5);
        TrackPoint out;
        CHECK(tb.read(3, out));
        CHECK_NEAR(out.x_ft, 3.0, 1e-6);
        CHECK(!tb.read(5, out));                    // not written yet
    }

    CASE("when it wraps, old points are refused rather than served torn");
    {
        TrackBuffer<TrackPoint, 8> tb;
        for (int i = 0; i < 20; ++i) {
            TrackPoint p;
            p.x_ft = static_cast<float>(i);
            tb.push(p);
        }
        CHECK(tb.full());
        CHECK(tb.size() == 8);
        CHECK(tb.total() == 20);
        TrackPoint out;
        CHECK(!tb.read(11, out));                   // overwritten
        CHECK(tb.read(12, out));                    // oldest still held
        CHECK_NEAR(out.x_ft, 12.0, 1e-6);
        CHECK(tb.read(19, out));
        CHECK_NEAR(out.x_ft, 19.0, 1e-6);

        // How the map screen walks it: oldest to newest, no index maths.
        for (size_t i = 0; i < tb.size(); ++i) {
            CHECK(tb.read_nth(i, out));
            CHECK_NEAR(out.x_ft, static_cast<double>(12 + i), 1e-6);
        }
    }

    CASE("clear() empties it");
    {
        TrackBuffer<TrackPoint, 4> tb;
        TrackPoint p;
        tb.push(p); tb.push(p);
        tb.clear();
        CHECK(tb.size() == 0);
        CHECK(!tb.read(0, p));
    }

    CASE("a reader never sees a half-written point while the writer runs");
    {
        // The map screen walks this buffer on core 1 while `gnss` appends on
        // core 0. Each point is self-consistent: y is always x negated and
        // t_ms is always x doubled, so a tear shows up as a broken triple.
        TrackBuffer<TrackPoint, 64> tb;
        std::atomic<bool> stop{false};
        std::atomic<int>  torn{0};
        std::atomic<int>  seen{0};

        std::thread writer([&] {
            for (uint32_t i = 0; i < 200000 && !stop.load(); ++i) {
                TrackPoint p;
                p.x_ft = static_cast<float>(i);
                p.y_ft = -static_cast<float>(i);
                p.t_ms = i * 2;
                tb.push(p);
            }
            stop.store(true);
        });

        std::thread reader([&] {
            TrackPoint p;
            while (!stop.load()) {
                for (size_t i = 0; i < tb.size(); ++i) {
                    if (!tb.read_nth(i, p)) continue;
                    seen.fetch_add(1);
                    if (p.y_ft != -p.x_ft ||
                        p.t_ms != static_cast<uint32_t>(p.x_ft) * 2) {
                        torn.fetch_add(1);
                    }
                }
            }
        });

        writer.join();
        reader.join();
        CHECK(seen.load() > 1000);          // the test actually exercised it
        CHECK(torn.load() == 0);
    }

    return check::report("state");
}
