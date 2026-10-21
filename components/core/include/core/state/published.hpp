// A lock-free single-writer / many-reader slot.
//
// Each producer task owns one Published<T> and is the ONLY thing that
// calls publish(). Any number of readers may call get() concurrently and
// always receive an internally consistent copy.
//
// This replaces v1's g_snapMutex + publishSnapshot() + the g_* mirror
// globals. Keep T trivially copyable and small — a few hundred bytes.
//
// Two buffers, not a seqlock. A seqlock reader that catches the writer
// mid-update spins until the writer finishes, and on one core a
// higher-priority reader that preempted a lower-priority writer there spins
// forever: ctrl (18) reading gnss (12) on core 0 was exactly that. Here the
// writer only ever fills the buffer readers are not being sent to, so a
// writer stopped half way through is invisible to them. A reader retries
// only if the writer *completed* a publish during its copy, which needs the
// writer to have made progress, so it cannot spin on a stalled one.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace core {

// A test seam: called between filling the spare buffer and flipping to it,
// the point where a preempting reader used to hang. Production uses this
// no-op, which compiles away.
struct NoPublishHook { static void mid() {} };

template <typename T, typename Hook = NoPublishHook>
class Published {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Published<T> requires a trivially copyable T");
    // Copied as 32-bit words through relaxed atomics: plain lw/sw on RV32,
    // and no formal data race for the reader's speculative copy.
    static constexpr size_t kWords = (sizeof(T) + 3) / 4;

    // Word i of a T, straight from or into its bytes. There is no whole-T
    // staging array: with get() inlined at -O2 that array and the T it was
    // copied into were two copies of the slot in the caller's frame, 312
    // bytes for Health, and logic_tick went over its 1 kB.
    static constexpr size_t bytes_in(size_t i) {
        return sizeof(T) - i * 4 < 4 ? sizeof(T) - i * 4 : 4;
    }
    static void store_words(std::atomic<uint32_t>* b, const T& v) {
        const auto* p = reinterpret_cast<const unsigned char*>(&v);
        for (size_t i = 0; i < kWords; ++i) {
            uint32_t w = 0;
            std::memcpy(&w, p + i * 4, bytes_in(i));
            b[i].store(w, std::memory_order_relaxed);
        }
    }
    static void load_words(const std::atomic<uint32_t>* b, T& out) {
        auto* p = reinterpret_cast<unsigned char*>(&out);
        for (size_t i = 0; i < kWords; ++i) {
            const uint32_t w = b[i].load(std::memory_order_relaxed);
            std::memcpy(p + i * 4, &w, bytes_in(i));
        }
    }

public:
    // Before the first publish a reader gets T{}, member defaults and all,
    // as it did when the slot was a plain T: not zero bytes.
    Published() { store_words(buf_[0], T{}); }

    // Writer task only. Never call this from two tasks.
    void publish(const T& v) {
        const uint32_t n = count_.load(std::memory_order_relaxed);
        // Orders the previous count store before these data stores, so a
        // reader that sees any of them also sees the count move on and
        // throws its copy away.
        std::atomic_thread_fence(std::memory_order_release);
        store_words(buf_[(n + 1) & 1u], v);
        Hook::mid();
        count_.store(n + 1, std::memory_order_release);
    }

    // Any reader, any task.
    // The copy lands in the returned T as it is read; a copy the writer
    // overtook is simply read again over the top of it.
    T get() const {
        T out;
        for (;;) {
            const uint32_t n = count_.load(std::memory_order_acquire);
            load_words(buf_[n & 1u], out);
            std::atomic_thread_fence(std::memory_order_acquire);
            // Unchanged: the writer has not started on this buffer since.
            // Do not "optimise" this second load away.
            if (count_.load(std::memory_order_relaxed) == n) return out;
        }
    }

    // Publishes so far. Cheap staleness check without copying the payload.
    uint32_t generation() const { return count_.load(std::memory_order_acquire); }

private:
    std::atomic<uint32_t> count_{0};
    std::atomic<uint32_t> buf_[2][kWords] = {};
};

}  // namespace core
