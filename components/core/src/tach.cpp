#include "core/tach/tach.hpp"

namespace core::tach {
namespace {
constexpr float kMmPerMile = 1609344.0f;
constexpr float kUsPerHour = 3600.0f * 1000000.0f;
constexpr float kUsPerMin  = 60.0f * 1000000.0f;
}  // namespace

float rpm_from_pulses(uint32_t pulses, uint32_t dt_us, uint8_t pulses_per_rev) {
    if (pulses == 0 || dt_us == 0 || pulses_per_rev == 0) return 0.0f;
    const float revs = static_cast<float>(pulses) / pulses_per_rev;
    return revs * kUsPerMin / static_cast<float>(dt_us);
}

float mph_from_pulses(uint32_t pulses, uint32_t dt_us, const WheelConfig& c) {
    if (pulses == 0 || dt_us == 0 || c.pulses_per_rev == 0) return 0.0f;
    const float revs = static_cast<float>(pulses) / c.pulses_per_rev;
    const float miles = revs * c.circumference_mm / kMmPerMile;
    return miles * kUsPerHour / static_cast<float>(dt_us);
}

void Meter::reset() {
    rpm_ = raw_rpm_ = 0.0f;
    running_ = false;
    started_ = false;
    rate_.reset();
}

void Meter::update(uint32_t total, uint64_t edge_us, uint64_t now_us,
                   const Config& c) {
    if (!started_) {
        last_total_ = total;
        last_edge_us_ = edge_us ? edge_us : now_us;
        started_ = true;
        return;
    }

    const uint32_t dp = total - last_total_;         // unsigned: wrap-safe

    if (dp > 0 && edge_us > last_edge_us_) {
        // The interval that matters is between the edge that ended the last
        // measurement and the edge that ended this one — not between polls.
        const uint64_t dt = edge_us - last_edge_us_;
        last_total_ = total;
        last_edge_us_ = edge_us;
        raw_rpm_ = rpm_from_pulses(dp, static_cast<uint32_t>(dt), c.pulses_per_rev) *
                   static_cast<float>(c.revs_per_pulse ? c.revs_per_pulse : 1);
        running_ = true;
        // dt / (dt + tau), as WheelMeter: 1 when tau is 0, so raw.
        const float dt_ms = static_cast<float>(dt) / 1000.0f;
        rpm_ += (raw_rpm_ - rpm_) * (dt_ms / (dt_ms + static_cast<float>(c.tau_ms)));
        rate_.add(edge_us, raw_rpm_, static_cast<uint32_t>(c.rate_window_ms) * 1000u);
        return;
    }

    // No pulses this tick. At 500 Hz that is normal even at 6000 rpm — one
    // pulse every 10 ms — so silence only means stalled once it has gone on
    // longer than any real idle.
    if ((now_us - last_edge_us_) / 1000u >= c.stall_ms) {
        running_ = false;
        raw_rpm_ = 0.0f;
        rpm_ = 0.0f;     // snapped, not decayed: a stopped engine reads zero
        rate_.reset();
    }
}

void RateEstimator::reset() {
    head_ = 0;
    n_ = 0;
    rate_ = 0.0f;
}

void RateEstimator::add(uint64_t t_us, float rpm, uint32_t window_us) {
    t_us_[head_] = t_us;
    rpm_[head_] = rpm;
    head_ = (head_ + 1) % kMaxSamples;
    if (n_ < kMaxSamples) ++n_;

    // Forget what has fallen out of the window, oldest first.
    while (n_ > 1) {
        const int oldest = (head_ - n_ + kMaxSamples) % kMaxSamples;
        if (t_us - t_us_[oldest] <= window_us) break;
        --n_;
    }
    // Three readings, or two intervals, before there is a slope to speak of.
    if (n_ < 3) {
        rate_ = 0.0f;
        return;
    }

    // Least squares, with time in seconds before the newest reading so the
    // sums stay small enough for a float.
    float st = 0, sv = 0, stt = 0, stv = 0;
    for (int k = 0; k < n_; ++k) {
        const int i = (head_ - 1 - k + kMaxSamples) % kMaxSamples;
        const float t = -static_cast<float>(t_us - t_us_[i]) * 1e-6f;
        st += t;
        sv += rpm_[i];
        stt += t * t;
        stv += t * rpm_[i];
    }
    const float n = static_cast<float>(n_);
    const float den = n * stt - st * st;
    if (den <= 0.0f) return;              // every reading at one instant
    float r = (n * stv - st * sv) / den;
    if (r > kMaxRpmPerS) r = kMaxRpmPerS;
    if (r < -kMaxRpmPerS) r = -kMaxRpmPerS;
    rate_ = r;
}

void WheelMeter::reset() {
    mph_ = out_ = 0.0f;
    started_ = false;
}

void WheelMeter::update(uint32_t total, uint64_t edge_us, uint64_t now_us,
                        const WheelConfig& c) {
    if (!started_) {
        last_total_ = total;
        last_edge_us_ = edge_us ? edge_us : now_us;
        started_ = true;
        return;
    }

    const uint32_t dp = total - last_total_;

    if (dp > 0 && edge_us > last_edge_us_) {
        const uint64_t dt = edge_us - last_edge_us_;
        last_total_ = total;
        last_edge_us_ = edge_us;
        const float raw = mph_from_pulses(dp, static_cast<uint32_t>(dt), c);
        // First-order low-pass with time constant tau over an interval dt:
        // dt / (dt + tau) is exp's first-order stand-in, and needs no expf.
        const float dt_ms = static_cast<float>(dt) / 1000.0f;
        const float alpha = dt_ms / (dt_ms + static_cast<float>(c.tau_ms));
        mph_ += (raw - mph_) * alpha;
        out_ = mph_;
        return;
    }

    if (now_us <= last_edge_us_) return;
    const uint64_t since = now_us - last_edge_us_;
    if (since / 1000u >= c.stall_ms) {
        mph_ = out_ = 0.0f;
        return;
    }
    const float bound = mph_from_pulses(1, static_cast<uint32_t>(since), c) * 1.1f;
    out_ = bound < mph_ ? bound : mph_;
}

}  // namespace core::tach
