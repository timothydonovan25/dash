// Bulk data that must not go in a slot (§4.4).
//
// A slot copies its whole payload on every publish and on every read. That
// is right for 40 bytes of engine data at 50 Hz and wrong for 2400 track
// points, which is why v1's 2.4 kB memcpy-per-publish existed and why it is
// not repeated here.
//
// One writer appends. Readers walk it by index. A reader that asks for an
// index the writer has since overwritten gets `false` rather than a torn
// point — at 10 Hz with a capacity of thousands that never happens in
// practice, but the map screen must not draw garbage the one time it does.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace core::state {

template <typename T, size_t N>
class TrackBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "TrackBuffer<T> requires a trivially copyable T");
    static_assert(N > 0, "TrackBuffer needs room for at least one element");

    // One slot more than the buffer holds. The writer is allowed to be
    // half-way through filling that spare while a reader copies the oldest
    // readable point, so the two never touch the same memory. Without it,
    // a reader taking the oldest point races the writer overwriting it, and
    // the after-the-fact sequence check cannot see the difference: the
    // writer has not bumped its counter yet, so the copy looks valid and is
    // torn. A concurrent stress test finds this in well under a second.
    static constexpr size_t kSlots = N + 1;

public:
    // Writer task only.
    void push(const T& v) {
        const size_t w = write_.load(std::memory_order_relaxed);
        buf_[w % kSlots] = v;
        std::atomic_thread_fence(std::memory_order_release);
        write_.store(w + 1, std::memory_order_relaxed);
    }

    void clear() { write_.store(0, std::memory_order_release); }

    // Total ever pushed. Wraps at 2^64 pushes, which at 10 Hz is longer
    // than the bike will exist.
    size_t total() const { return write_.load(std::memory_order_acquire); }

    // How many are still readable.
    size_t size() const {
        const size_t t = total();
        return t < N ? t : N;
    }

    static constexpr size_t capacity() { return N; }
    bool full() const { return total() >= N; }

    // Read by absolute index, 0 being the first ever pushed. False if that
    // point has been overwritten, or has not been written yet.
    bool read(size_t index, T& out) const {
        const size_t w = write_.load(std::memory_order_acquire);
        if (index >= w) return false;
        if (w - index > N) return false;                 // long gone
        out = buf_[index % kSlots];
        std::atomic_thread_fence(std::memory_order_acquire);
        // The writer may have lapped us mid-copy. It is inside our slot only
        // once it has advanced a full kSlots past us, so that is the bound.
        return write_.load(std::memory_order_relaxed) - index < kSlots;
    }

    // Read the i-th of the currently readable points, oldest first. This is
    // what a map screen wants: `for (i = 0; i < size(); ++i)`.
    bool read_nth(size_t i, T& out) const {
        const size_t w = total();
        const size_t first = w > N ? w - N : 0;
        return read(first + i, out);
    }

private:
    T                   buf_[kSlots] = {};
    std::atomic<size_t> write_{0};
};

// What the trip map and the track map record. Sixteen bytes, so 4000 points
// is 64 kB — comfortable in PSRAM, and about 6 minutes at 10 Hz.
struct TrackPoint {
    float    x_ft = 0.0f;      // east of the session origin
    float    y_ft = 0.0f;      // north of the session origin
    float    speed_mph = 0.0f;
    uint32_t t_ms = 0;
};

}  // namespace core::state
