// When a GPS fix was taken, on the system clock.
//
// The lap timer used Gnss::rx_ms: the system clock when the fix's first
// sentence finished arriving. That is the fix's time plus the receiver's
// output latency (which varies fix to fix with what it computed), the
// sentence's time on the UART, and up to one 5 ms poll of `gnss`. A lap
// time is the difference of two crossings, so both crossings' jitter is in
// it.
//
// Every fix also says when it was taken, in GPS time (Gnss::utc_ms, exact to
// the receiver's epoch). So this keeps the offset between that clock and the
// system clock, and stamps each fix as utc + offset. The offset is the
// *smallest* rx - utc seen, as NTP keeps the lowest-delay sample: every
// sample is the true offset plus a latency that is never negative, so the
// minimum is the one with the least of it. It is allowed to creep upward by
// `kSlewUs` a fix, which covers the two crystals drifting apart (the P4's is
// tens of ppm) without letting one slow sentence move it.
//
// What the stamp is not: the instant the satellites were measured. It is
// that plus the receiver's quickest output, a constant; constants cancel out
// of a lap time. What it removes is the part that varies.
#pragma once
#include <cstdint>

namespace core::fixtime {

class Clock {
public:
    // 100 ppm at 10 Hz. A larger drift than this and the stamps lag a little;
    // they never jump.
    static constexpr int32_t kSlewUs = 10;
    // GPS time disagreeing with the system clock by more than this since the
    // last fix (a receiver that reset, a time-of-day that jumped) starts the
    // offset again from the next sample.
    static constexpr int32_t kResyncMs = 1000;

    // A fix whose sentence said `utc_ms` (time of day, 0..86,399,999) and
    // finished arriving at `rx_ms` on the system clock. Returns when it was
    // taken, on the system clock: never later than rx_ms.
    uint32_t stamp(uint32_t utc_ms, uint32_t rx_ms);

    // How much later than the stamp the last fix arrived: the receiver's
    // latency above its quickest, plus the UART and the poll. For DIAG and
    // tests.
    uint32_t last_lag_ms() const { return last_lag_ms_; }
    bool     locked() const { return locked_; }
    void     reset() { locked_ = false; }

private:
    bool     locked_ = false;
    uint32_t last_utc_ = 0;     // last time of day seen
    uint32_t last_rx_ = 0;
    uint32_t utc_cont_ = 0;     // time of day made continuous across midnight
    int64_t  offset_us_ = 0;    // system - utc_cont, in microseconds
    uint32_t last_lag_ms_ = 0;
};

}  // namespace core::fixtime
