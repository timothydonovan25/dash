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

    CASE("a record formats as a CSV line anyone can open");
    {
        Record r;
        r.t_ms = 12345;
        r.speed_mph = 54.2f;
        r.rpm = 7250.0f;
        r.lean_deg = -32.5f;
        r.pitch_deg = 3.0f;
        r.g_lat = 0.62f;
        r.g_vert = 1.18f;
        r.lat = 42.501234;
        r.lon = -83.123456;
        r.lap_ms = 42150;
        r.sats = 11;
        r.flags = kFlagFix | kFlagTiming;

        char line[256];
        const size_t n = format_csv(line, sizeof line, r);
        CHECK(n > 0);
        CHECK(line[n] == '\0');
        CHECK(line[n - 1] == '\n');
        CHECK(contains(line, "12345,"));
        CHECK(contains(line, "54.2,"));
        CHECK(contains(line, "-32.5,"));
        // Position keeps six decimals. A float would quantise this to about
        // thirty feet and the trace would not show your line through a turn.
        CHECK(contains(line, "42.501234"));
        CHECK(contains(line, "-83.123456"));

        char head[256];
        CHECK(csv_header(head, sizeof head) > 0);
        CHECK(contains(head, "t_ms,speed_mph"));
        CHECK(contains(head, "lat,lon"));
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

    return check::report("log");
}
