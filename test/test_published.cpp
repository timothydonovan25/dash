#include "check.hpp"
#include "core/state/published.hpp"
#include <thread>
#include <atomic>
using core::Published;

// A payload big enough that a torn read would be obvious. Every field
// carries the same counter, so any inconsistency is detectable.
struct Wide {
    uint32_t a[16];
    bool consistent() const {
        for (int i = 1; i < 16; ++i) if (a[i] != a[0]) return false;
        return true;
    }
};

int main() {
    CASE("a fresh slot reads as a default-constructed value");
    Published<int> p;
    CHECK(p.get() == 0);
    CHECK(p.generation() == 0);

    CASE("publish then get round-trips");
    p.publish(42);
    CHECK(p.get() == 42);
    CHECK(p.generation() == 1);

    CASE("generation counts publishes");
    for (int i = 0; i < 10; ++i) p.publish(i);
    CHECK(p.generation() == 11);
    CHECK(p.get() == 9);

    CASE("a reader never observes a torn value under contention");
    Published<Wide> w;
    std::atomic<bool> stop{false};
    std::atomic<int>  torn{0};
    std::atomic<long> reads{0};

    std::thread writer([&] {
        for (uint32_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
            Wide v;
            for (int i = 0; i < 16; ++i) v.a[i] = n;
            w.publish(v);
        }
    });

    std::thread readers[3];
    for (auto& t : readers) t = std::thread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!w.get().consistent()) torn.fetch_add(1);
            reads.fetch_add(1);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop = true;
    writer.join();
    for (auto& t : readers) t.join();

    std::printf("       (%ld reads across 3 threads)\n", reads.load());
    CHECK(reads.load() > 1000);      // the test actually exercised something
    CHECK(torn.load() == 0);

    return check::report("published");
}
