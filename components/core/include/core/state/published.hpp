// A lock-free single-writer / many-reader slot.
//
// Each producer task owns one Published<T> and is the ONLY thing that
// calls publish(). Any number of readers may call get() concurrently and
// always receive an internally consistent copy.
//
// This replaces v1's g_snapMutex + publishSnapshot() + the g_* mirror
// globals. Keep T trivially copyable and small — a few hundred bytes.
#pragma once
#include <atomic>
#include <type_traits>

namespace core {

template <typename T>
class Published {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Published<T> requires a trivially copyable T");
public:
    // Writer task only. Never call this from two tasks.
    void publish(const T& v) {
        seq_.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        data_ = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_relaxed);
    }

    // Any reader, any task. Retries if it catches a write in progress.
    //
    // A reader CAN copy a torn value here — that is expected. What makes
    // it safe is that the sequence check afterwards detects the tear and
    // the copy is thrown away and retried. Do not "optimise" the second
    // load away.
    T get() const {
        for (;;) {
            const uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1u) continue;                     // writer is mid-update
            T out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == a) return out;
        }
    }

    // Cheap staleness check without copying the payload.
    uint32_t generation() const { return seq_.load(std::memory_order_acquire) >> 1; }

private:
    std::atomic<uint32_t> seq_{0};
    T data_{};
};

}  // namespace core
