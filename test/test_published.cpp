#include "check.hpp"
#include "core/state/published.hpp"
#include <thread>
#include <atomic>
using core::Published;

// A payload big enough that a torn read would be obvious. Every field
// carries the same counter, so any inconsistency is detectable.
// A writer "preempted" between filling its buffer and publishing it: the
// hook runs a read right there, which is what a higher-priority reader on
// the same core sees. The seqlock this replaced spun forever at this point.
struct MidPublish {
    static inline void (*fn)() = nullptr;
    static void mid() { if (fn) fn(); }
};

struct Defaults {
    int   a = 7;
    float b = 1.5f;
};

// Not a whole number of words: the last word is partly padding, and only
// sizeof(T) bytes of it may be read or written.
struct Odd {
    uint8_t c[7] = {1, 2, 3, 4, 5, 6, 7};
};

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

    CASE("a slot reads its member defaults before the first publish");
    Published<Defaults> d;
    CHECK(d.get().a == 7);
    CHECK(d.get().b == 1.5f);

    CASE("a size that is not a whole number of words round-trips");
    {
        Published<Odd> o;
        CHECK(o.get().c[0] == 1 && o.get().c[6] == 7);
        Odd v;
        for (int i = 0; i < 7; ++i) v.c[i] = static_cast<uint8_t>(200 + i);
        o.publish(v);
        const Odd r = o.get();
        bool same = true;
        for (int i = 0; i < 7; ++i) same = same && r.c[i] == 200 + i;
        CHECK(same);
        static_assert(sizeof(Odd) == 7, "the case is about a ragged last word");
    }

    CASE("a read inside an unfinished publish returns the last completed value");
    static Published<int, MidPublish> s;
    static int seen = -1;
    s.publish(1);
    MidPublish::fn = [] { seen = s.get(); };
    s.publish(2);
    MidPublish::fn = nullptr;
    CHECK(seen == 1);
    CHECK(s.get() == 2);
    CHECK(s.generation() == 2);

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
