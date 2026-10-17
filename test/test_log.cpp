#include "check.hpp"
#include "core/log/log.hpp"
#include <atomic>
#include <cstring>
#include <thread>

using namespace core::log;

namespace {
bool contains(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}
}  // namespace

int main() {
    CASE("records round-trip in order");
    {
        Ring<Record, 8> ring;
        CHECK(ring.empty());
        for (int i = 0; i < 5; ++i) {
            Record r;
            r.t_ms = static_cast<uint32_t>(i * 20);
            r.rpm = static_cast<float>(i * 100);
            CHECK(ring.push(r));
        }
        CHECK(ring.size() == 5);
        for (int i = 0; i < 5; ++i) {
            Record out;
            CHECK(ring.pop(out));
            CHECK(out.t_ms == static_cast<uint32_t>(i * 20));
        }
        CHECK(ring.empty());
        CHECK(!ring.pop(*(new Record{})));    // nothing left
    }

    CASE("a full ring drops the newest and says so");
    {
        // Deliberately not overwriting the oldest. A blackbox missing its
        // start tells you nothing about how the incident began, and the
        // producer must never block on a card that has stalled.
        Ring<Record, 4> ring;
        Record r;
        for (int i = 0; i < 4; ++i) {
            r.t_ms = static_cast<uint32_t>(i);
            CHECK(ring.push(r));
        }
        r.t_ms = 999;
        CHECK(!ring.push(r));
        CHECK(ring.dropped() == 1);

        Record out;
        ring.pop(out);
        CHECK(out.t_ms == 0);                 // the oldest survived
        CHECK(ring.push(r));                  // and there is room again now
    }

    CASE("batch drain empties what is there and no more");
    {
        Ring<Record, 64> ring;
        for (int i = 0; i < 10; ++i) {
            Record r;
            r.t_ms = static_cast<uint32_t>(i);
            ring.push(r);
        }
        Record batch[32];
        CHECK(ring.pop_batch(batch, 32) == 10);
        CHECK(batch[0].t_ms == 0);
        CHECK(batch[9].t_ms == 9);
        CHECK(ring.pop_batch(batch, 32) == 0);
    }

    CASE("a batch smaller than the backlog leaves the rest");
    {
        Ring<Record, 64> ring;
        for (int i = 0; i < 10; ++i) { Record r; r.t_ms = i; ring.push(r); }
        Record batch[4];
        CHECK(ring.pop_batch(batch, 4) == 4);
        CHECK(ring.size() == 6);
        CHECK(batch[3].t_ms == 3);
    }

    CASE("clear() throws away the backlog without losing the producer");
    {
        Ring<Record, 8> ring;
        for (int i = 0; i < 5; ++i) { Record r; ring.push(r); }
        ring.clear();
        CHECK(ring.empty());
        Record r;
        CHECK(ring.push(r));
    }

    CASE("the producer never blocks and the consumer never tears");
    {
        // `logic` on core 0 writing while `logger` on core 1 drains to a card
        // that stalls. Each record is self-consistent — rpm is t_ms doubled —
        // so a tear is detectable, and the producer must never stop.
        Ring<Record, 256> ring;
        std::atomic<bool> stop{false};
        std::atomic<int>  torn{0};
        std::atomic<long> seen{0};

        std::thread producer([&] {
            for (uint32_t i = 0; i < 200000; ++i) {
                Record r;
                r.t_ms = i;
                r.rpm = static_cast<float>(i * 2);
                ring.push(r);              // drops are fine, stalls are not
            }
            stop.store(true);
        });

        std::thread consumer([&] {
            Record batch[64];
            while (!stop.load() || !ring.empty()) {
                const size_t n = ring.pop_batch(batch, 64);
                for (size_t i = 0; i < n; ++i) {
                    if (batch[i].rpm != static_cast<float>(batch[i].t_ms) * 2.0f) {
                        torn.fetch_add(1);
                    }
                    seen.fetch_add(1);
                }
            }
        });

        producer.join();
        consumer.join();
        CHECK(seen.load() > 1000);
        CHECK(torn.load() == 0);
    }

    CASE("a record formats as a CSV line RaceRender can read");
    {
        Record r;
        r.t_ms = 12345;
        r.speed_mph = 54.2f;
        r.rpm = 7250.0f;
        r.lean_deg = -32.5f;
        r.pitch_deg = 3.0f;
        r.g_lat = 0.62f;
        r.g_lon = -0.45f;
        r.g_vert = 1.18f;
        r.course_deg = 191.0f;
        r.lat = 42.501234;
        r.lon = -83.123456;
        r.lap_ms = 42150;
        r.lap_num = 3;
        r.gear = 4;
        r.sats = 11;
        r.flags = kFlagFix | kFlagTiming;

        char line[256];
        const size_t n = format_csv(line, sizeof line, r);
        CHECK(n > 0);
        CHECK(line[n] == '\0');
        CHECK(line[n - 1] == '\n');

        // Seconds, not milliseconds. RaceRender reads this column as
        // seconds, so 12345 here would import a twelve-second sample as
        // three and a half hours and stretch the whole video with it.
        CHECK(contains(line, "12.345,"));
        CHECK(!contains(line, "12345,"));

        CHECK(contains(line, "54.2,"));
        CHECK(contains(line, "-32.5,"));
        // X before Y: accel and braking, then lateral.
        CHECK(contains(line, "-0.45,0.62,"));
        // Position keeps six decimals. A float would quantise this to about
        // thirty feet and the trace would not show your line through a turn.
        CHECK(contains(line, "42.501234"));
        CHECK(contains(line, "-83.123456"));

        char head[256];
        CHECK(csv_header(head, sizeof head) > 0);
        // The marker has to be the very first thing in the file or the lap
        // tags and column names are ignored.
        CHECK(head[0] == '#');
        CHECK(contains(head, "# RaceRender Data\n"));
        CHECK(contains(head, "\nTime,Latitude,Longitude,MPH,RPM,X,Y,"));

        // The replay columns, at the end where RaceRender ignores them, and
        // one cell per name.
        CHECK(contains(head, ",Flags,FrontMPH,RearMPH,GpsMPH,HDOP,GpsUtc,FusedMPH,FusedSigma,AltFt\n"));
        const auto commas = [](const char* p) {
            size_t c = 0;
            for (; *p && *p != '\n'; ++p) c += *p == ',';
            return c;
        };
        CHECK(commas(line) == commas(std::strchr(head, '\n') + 1));
        r.front_mph = 31.25f;
        r.gps_utc_ms = 45296120;              // 12:34:56.12
        r.fused_mph = 30.5f;
        r.alt_ft = 5280.4f;
        format_csv(line, sizeof line, r);
        CHECK(contains(line, ",5280\n"));                // last, whole feet
        CHECK(contains(line, ",31.25,"));
        CHECK(contains(line, ",45296.12,"));
        CHECK(contains(line, ",30.50,"));
    }

    CASE("a log is named by session until the date is known, then by local date");
    {
        char name[40];
        CHECK(log_file_name(name, sizeof name, Date{}, 7) > 0);
        CHECK(std::strcmp(name, "logs/s007.csv") == 0);
        CHECK(log_file_name(name, sizeof name, Date{2026, 10, 13}, 7) > 0);
        CHECK(std::strcmp(name, "logs/2026-10-13_s007.csv") == 0);
        CHECK(log_file_name(name, 10, Date{2026, 10, 13}, 7) == 0);   // too small
    }

    CASE("the session number comes back out of either form of name");
    {
        CHECK(session_of("s007.csv") == 7);
        CHECK(session_of("2026-10-13_s042.csv") == 42);
        CHECK(session_of("notes.csv") == -1);
        CHECK(session_of("xs007.csv") == -1);
        CHECK(session_of("s07.csv") == -1);
        CHECK(session_of("s007.txt") == -1);
    }

    CASE("the local date crosses midnight, months and years the right way");
    {
        // 02:30 UTC on 14 October is still the 13th five hours west.
        Date d = local_date(Date{2026, 10, 14}, 9000000u, -5);
        CHECK(d.year == 2026 && d.month == 10 && d.day == 13);
        // ...and the 1st of a month goes back to the last of the one before.
        d = local_date(Date{2026, 3, 1}, 3600000u, -5);
        CHECK(d.month == 2 && d.day == 28);
        d = local_date(Date{2024, 3, 1}, 3600000u, -5);           // leap year
        CHECK(d.month == 2 && d.day == 29);
        d = local_date(Date{2027, 1, 1}, 0u, -1);
        CHECK(d.year == 2026 && d.month == 12 && d.day == 31);
        // East of UTC late in the day is tomorrow.
        d = local_date(Date{2026, 12, 31}, 23u * 3600000u, 2);
        CHECK(d.year == 2027 && d.month == 1 && d.day == 1);
        // Same day either side when nothing crosses.
        d = local_date(Date{2026, 10, 13}, 18u * 3600000u, -5);
        CHECK(d.day == 13);
    }

    CASE("a lap tag is hh:mm:ss.nn, which RaceRender parses strictly");
    {
        char tag[64];

        // 2m43.08s, the example from the format documentation.
        CHECK(format_lap_marker(tag, sizeof tag, 0, 163080) > 0);
        CHECK(contains(tag, "# Lap 0: 00:02:43.08\n"));

        // Every field zero padded, including the hours nobody will ever use.
        CHECK(format_lap_marker(tag, sizeof tag, 7, 5000) > 0);
        CHECK(contains(tag, "# Lap 7: 00:00:05.00\n"));

        // Hundredths truncate rather than round up into a whole second,
        // which would make a lap read one hundredth quicker than it was.
        CHECK(format_lap_marker(tag, sizeof tag, 1, 3999) > 0);
        CHECK(contains(tag, "# Lap 1: 00:00:03.99\n"));

        // Over an hour, because a trail ride is not a track session.
        CHECK(format_lap_marker(tag, sizeof tag, 2, 3723456) > 0);
        CHECK(contains(tag, "# Lap 2: 01:02:03.45\n"));
    }

    CASE("every offered rate divides the 50 Hz logic tick");
    {
        // The reason the menu is a list and not a Hz field: a rate the tick
        // cannot divide evenly logs alternating 20 ms and 40 ms gaps.
        for (uint8_t r = 1; r < kRateCount; ++r) {
            const uint32_t ms = rate_interval_ms(r);
            CHECK(ms > 0);
            CHECK(ms % 20 == 0);
        }
        CHECK(rate_interval_ms(static_cast<uint8_t>(Rate::Off)) == 0);
        CHECK(rate_interval_ms(static_cast<uint8_t>(Rate::Hz50)) == 20);
        CHECK(rate_interval_ms(static_cast<uint8_t>(Rate::Hz1)) == 1000);

        // A value from a firmware that knew more rates falls back to the
        // default, rather than silently turning the blackbox off.
        CHECK(rate_interval_ms(kRateCount) ==
              rate_interval_ms(static_cast<uint8_t>(kDefaultRate)));

        // Every option has a label; the menu indexes this array directly.
        for (uint8_t r = 0; r < kRateCount; ++r) CHECK(kRateLabels[r] != nullptr);
        CHECK(contains(kRateLabels[0], "Off"));
    }

    CASE("the decimator logs the first tick, then holds the rate");
    {
        Decimator g;
        const uint8_t hz10 = static_cast<uint8_t>(Rate::Hz10);

        CHECK(g.due(hz10, 1000));            // primes on the first call
        CHECK(!g.due(hz10, 1020));
        CHECK(!g.due(hz10, 1080));
        CHECK(g.due(hz10, 1100));            // 100 ms later
        CHECK(!g.due(hz10, 1120));

        // 50 Hz has to fire on every 20 ms tick. The slack is what makes a
        // clock that reads 19 ms of elapsed time still count as due — without
        // it the top rate would quietly log at half its name.
        Decimator fast;
        const uint8_t hz50 = static_cast<uint8_t>(Rate::Hz50);
        CHECK(fast.due(hz50, 0));
        CHECK(fast.due(hz50, 19));
        CHECK(fast.due(hz50, 39));
    }

    CASE("a lap event is logged whatever the rate, unless logging is off");
    {
        Decimator g;
        const uint8_t hz1 = static_cast<uint8_t>(Rate::Hz1);
        CHECK(g.due(hz1, 0));
        CHECK(!g.due(hz1, 200));
        // The lap marker rides in the record, so dropping this tick would
        // lose the lap from the file altogether.
        CHECK(g.due(hz1, 300, /*force=*/true));
        // ...and the forced row restarts the cadence rather than being extra.
        CHECK(!g.due(hz1, 900));
        CHECK(g.due(hz1, 1300));

        // Off means off. A rider who turned logging off does not get a row
        // because a lap happened to end.
        Decimator off;
        const uint8_t none = static_cast<uint8_t>(Rate::Off);
        CHECK(!off.due(none, 0));
        CHECK(!off.due(none, 5000, /*force=*/true));
    }

    CASE("reset makes the next tick due, whenever it comes");
    {
        Decimator g;
        const uint8_t hz1 = static_cast<uint8_t>(Rate::Hz1);
        CHECK(g.due(hz1, 10000));
        CHECK(!g.due(hz1, 10020));
        g.reset();                            // rate changed, or a new card
        CHECK(g.due(hz1, 10040));
    }

    CASE("a short buffer truncates and still terminates");
    {
        Record r;
        r.t_ms = 987654;
        char small[8];
        const size_t n = format_csv(small, sizeof small, r);
        CHECK(n == sizeof small - 1);
        CHECK(small[sizeof small - 1] == '\0');
    }

    CASE("flush: a minute of riding is a write every 2 s and a sync every 7 s");
    {
        // The logger's loop as it runs: a pass every 20 ms, a row every
        // 100 ms (10 Hz, the default). Before this policy each empty-ring pass
        // wrote and synced: about 4.5 of each a second.
        FlushPolicy p;
        const FlushConfig c;
        int writes = 0, syncs = 0;
        uint32_t oldest_uncommitted = 0;    // row time of the oldest row not yet synced
        bool any_uncommitted = false;
        uint32_t worst_wait = 0;
        p.decide(0, false, c);              // riding from the start
        for (uint32_t t = 20; t <= 60000; t += 20) {
            if (t % 100 == 0) {
                p.buffered(t);
                if (!any_uncommitted) { any_uncommitted = true; oldest_uncommitted = t; }
            }
            const Flush f = p.decide(t, false, c);
            if (f != Flush::None) { p.written(t); ++writes; }
            if (f == Flush::WriteAndSync) {
                p.synced();
                ++syncs;
                if (any_uncommitted && t - oldest_uncommitted > worst_wait) {
                    worst_wait = t - oldest_uncommitted;
                }
                any_uncommitted = false;
            }
        }
        // A write every 2 s of rows, and every ~7 s (the sync clock starts at
        // the first write, 2 s after the first row) one more that also syncs:
        // about 33 writes and 8 syncs a minute, against ~270 of each before.
        CHECK(writes >= 30 && writes <= 35);
        CHECK(syncs >= 8 && syncs <= 9);
        CHECK(worst_wait <= c.max_age_ms + c.sync_every_ms + 100);
    }

    CASE("flush: stopping commits the ride at once");
    {
        FlushPolicy p;
        const FlushConfig c;
        CHECK(p.decide(0, false, c) == Flush::None);
        p.buffered(100);
        CHECK(p.decide(500, false, c) == Flush::None);     // half a second old
        CHECK(p.decide(520, true, c) == Flush::WriteAndSync);
        p.written(520);
        p.synced();
        // Still stopped, still logging: back to the timers, no sync per pass.
        p.buffered(600);
        CHECK(p.decide(700, true, c) == Flush::None);
        CHECK(p.decide(2600, true, c) == Flush::Write);
    }

    CASE("flush: nothing pending, nothing to do");
    {
        FlushPolicy p;
        const FlushConfig c;
        // Boots parked: no edge, nothing written.
        CHECK(p.decide(0, true, c) == Flush::None);
        CHECK(p.decide(10000, false, c) == Flush::None);
        // A stop with nothing buffered or uncommitted is not a sync.
        CHECK(p.decide(20000, true, c) == Flush::None);
        CHECK(!p.has_rows() && !p.unsynced());
    }

    CASE("flush: a block written when full starts the sync clock");
    {
        // The logger writes a full 8 kB block itself, outside decide(). The
        // sync is still owed sync_every_ms after it.
        FlushPolicy p;
        const FlushConfig c;
        p.decide(0, false, c);
        p.buffered(1000);
        p.written(1500);                           // block filled
        CHECK(p.decide(6000, false, c) == Flush::None);
        CHECK(p.decide(6500, false, c) == Flush::WriteAndSync);
    }

    CASE("flush: the millisecond counter wrapping does not stall or fire early");
    {
        FlushPolicy p;
        const FlushConfig c;
        const uint32_t t0 = 0xFFFFFF00u;
        p.decide(t0, false, c);
        p.buffered(t0);
        CHECK(p.decide(t0 + 1000, false, c) == Flush::None);
        CHECK(p.decide(t0 + 2000, false, c) == Flush::Write);
    }

    return check::report("log");
}
