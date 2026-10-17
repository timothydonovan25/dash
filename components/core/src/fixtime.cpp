#include "core/fixtime/fixtime.hpp"

namespace core::fixtime {
namespace {
constexpr int32_t kDayMs = 86400000;
}

uint32_t Clock::stamp(uint32_t utc_ms, uint32_t rx_ms) {
    if (locked_) {
        // Time of day moved on by this much; across midnight it went down.
        int32_t d_utc = static_cast<int32_t>(utc_ms) - static_cast<int32_t>(last_utc_);
        if (d_utc < -kDayMs / 2) d_utc += kDayMs;
        else if (d_utc > kDayMs / 2) d_utc -= kDayMs;
        const int32_t d_rx = static_cast<int32_t>(rx_ms - last_rx_);
        // The two clocks agree on how long it has been, give or take the
        // latency's spread. If not, one of them jumped: start again. A new
        // fix whose time has not moved has no time to go by (a receiver
        // without one yet reports zero): each re-lock stamps it on arrival.
        const int32_t disagree = d_rx - d_utc;
        if (d_utc <= 0 || disagree > kResyncMs || disagree < -kResyncMs) {
            locked_ = false;
        } else {
            utc_cont_ += static_cast<uint32_t>(d_utc);
        }
    }
    last_utc_ = utc_ms;
    last_rx_ = rx_ms;

    // Unsigned difference, so the system clock wrapping (49 days) and the
    // continuous time of day wrapping do not matter.
    if (!locked_) {
        utc_cont_ = utc_ms;
        offset_us_ = static_cast<int64_t>(static_cast<int32_t>(rx_ms - utc_cont_)) * 1000;
        locked_ = true;
    } else {
        const int64_t sample_us =
            static_cast<int64_t>(static_cast<int32_t>(rx_ms - utc_cont_)) * 1000;
        if (sample_us < offset_us_) {
            offset_us_ = sample_us;           // a quicker fix: believe it at once
        } else {
            const int64_t up = sample_us - offset_us_;
            offset_us_ += up < kSlewUs ? up : kSlewUs;
        }
    }

    // Rounded to the nearest millisecond, which is what every t_ms is.
    const int64_t off_ms = (offset_us_ >= 0 ? offset_us_ + 500 : offset_us_ - 500) / 1000;
    uint32_t t = utc_cont_ + static_cast<uint32_t>(static_cast<int32_t>(off_ms));
    // Never after it arrived: a stamp in the future would make the lap's
    // running time step backwards when the fix lands.
    if (static_cast<int32_t>(rx_ms - t) < 0) t = rx_ms;
    last_lag_ms_ = rx_ms - t;
    return t;
}

}  // namespace core::fixtime
