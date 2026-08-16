// The blackbox ring buffer (§9.2).
//
// Producers write fixed-size records and never touch the filesystem, never
// block, and never wait on the card. The `logger` task drains the ring to SD
// in big blocks. That split is the whole design: an SD card can stall for
// hundreds of milliseconds when its controller decides to do wear levelling,
// and `ctrl` has a 3 ms budget.
//
// So the contract here is deliberately lopsided: **a full buffer drops the
// newest record and counts it.** It does not block the producer, and it does
// not overwrite the oldest — a blackbox whose start is missing tells you
// nothing about how the incident began.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace core::log {

// One row of the blackbox. Fixed size on purpose: the drain writes whole
// records, and a variable-length format means a partial write at power-off
// corrupts the tail rather than truncating it.
struct Record {
    uint32_t t_ms      = 0;
    float    speed_mph = 0.0f;
    float    rpm       = 0.0f;
    float    lean_deg  = 0.0f;
    float    pitch_deg = 0.0f;
    float    g_lat     = 0.0f;
    float    g_vert    = 0.0f;
    double   lat       = 0.0;
    double   lon       = 0.0;
    uint32_t lap_ms    = 0;
    uint16_t flags     = 0;
    uint8_t  sats      = 0;
    uint8_t  gear_hint = 0;
};

enum Flags : uint16_t {
    kFlagNeutral  = 1u << 0,
    kFlagAirborne = 1u << 1,
    kFlagFix      = 1u << 2,
    kFlagTiming   = 1u << 3,
    kFlagEcu      = 1u << 4,
};

// Single producer, single consumer, lock-free. The producer is `logic`; the
// consumer is `logger` on the other core.
template <typename T, size_t N>
class Ring {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Ring<T> requires a trivially copyable T");
    static_assert(N >= 2, "Ring needs at least two slots");

public:
    // Producer only. False means the record was dropped because the drain
    // has fallen behind — which is information, not an error to retry.
    bool push(const T& v) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= N) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[w % N] = v;
        std::atomic_thread_fence(std::memory_order_release);
        write_.store(w + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer only.
    bool pop(T& out) {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        out = buf_[r % N];
        std::atomic_thread_fence(std::memory_order_acquire);
        read_.store(r + 1, std::memory_order_relaxed);
        return true;
    }

    // Drain into a caller's array. The logger writes 8 kB at a time rather
    // than a record at a time, because an SD card charges the same latency
    // for 512 bytes as for 8 kB.
    size_t pop_batch(T* out, size_t max) {
        size_t n = 0;
        while (n < max && pop(out[n])) ++n;
        return n;
    }

    size_t size() const {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_acquire);
    }
    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return N; }
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void clear() {
        read_.store(write_.load(std::memory_order_acquire),
                    std::memory_order_release);
    }

private:
    T                     buf_[N] = {};
    std::atomic<size_t>   write_{0};
    std::atomic<size_t>   read_{0};
    std::atomic<uint32_t> dropped_{0};
};

// A CSV line for one record, into a caller buffer. Written on the logger
// task, never on a producer. CSV rather than a packed binary format because
// the point of a blackbox you never open is nothing at all — this one opens
// in any spreadsheet on the card.
size_t format_csv(char* buf, size_t n, const Record& r);
size_t csv_header(char* buf, size_t n);

}  // namespace core::log
