#include "core/battery/battery.hpp"

namespace core::battery {

namespace {
// Charge is smoothed: a reading is a level, not a sample.
constexpr float kAlpha = 0.05f;
// More than this below the rest window's average is something drawing
// current -- the starter before its first tach edge -- not the battery.
constexpr float kLoadDropV = 0.5f;
float smooth(float avg, float v) { return avg == 0.0f ? v : avg + kAlpha * (v - avg); }
}  // namespace

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Unknown:      return "--";
        case Verdict::Good:         return "good";
        case Verdict::Weak:         return "weak";
        case Verdict::NotCharging:  return "not charging";
        case Verdict::Overcharging: return "overcharging";
    }
    return "?";
}

void Monitor::update(float vin_v, float low_v, float rpm, uint32_t t_ms, const Config& c) {
    if (vin_v < c.bike_min_v) {
        // USB, or nothing: not a battery reading. Start over when the bike
        // supply comes back, so a stale bucket is not mistaken for a dip.
        started_ = false;
        cranking_ = false;
        charging_ = false;
        return;
    }
    if (!started_) {
        started_ = true;
        started_ms_ = t_ms;
        running_ = rpm > 0.0f;
        // Key-on with the engine already turning is the dash restarting
        // mid-ride: no rest to be had this time.
        rest_open_ = !running_;
        rest_sum_ = 0.0f;
        rest_n_ = 0;
        load_v_ = low_v;
        last_ms_ = t_ms;
        bucket_[0] = bucket_[1] = low_v;
        bucket_ms_ = t_ms;
    }

    // Smoothed by time, not by sample, so it means the same at any rate.
    // Coming down from the resting level, a short inrush pulls this only a
    // fraction of the way; a battery that sags for the whole crank pulls it
    // all the way.
    const uint32_t dt = t_ms - last_ms_;
    last_ms_ = t_ms;
    const float a = c.crank_avg_ms == 0
                        ? 1.0f
                        : static_cast<float>(dt) / static_cast<float>(c.crank_avg_ms + dt);
    load_v_ += a * (low_v - load_v_);

    // The last second's low, in half-second buckets.
    if (t_ms - bucket_ms_ >= 500) {
        bucket_[1] = bucket_[0];
        bucket_[0] = load_v_;
        bucket_ms_ = t_ms;
    } else if (load_v_ < bucket_[0]) {
        bucket_[0] = load_v_;
    }

    const bool running = rpm > 0.0f;
    if (running != running_) {
        running_ = running;
        if (running) {
            // The engine has just been seen turning. The starter was on for
            // a moment before the first tach edge, so the dip may already
            // be in the buckets.
            cranking_ = true;
            crank_start_ms_ = t_ms;
            crank_min_ = bucket_[0] < bucket_[1] ? bucket_[0] : bucket_[1];
        }
    }

    if (cranking_) {
        if (load_v_ < crank_min_) crank_min_ = load_v_;
        if (t_ms - crank_start_ms_ >= c.crank_window_ms) {
            cranking_ = false;
            out_.crank_min_v = crank_min_;      // this start's, not the lowest ever
        }
    }

    // Rest, once per key-on: the mean over the window after the prime. The
    // engine turning, or a sudden drop below the window's own average (the
    // starter before its first tach edge), ends it unread: a rest voltage
    // with a load on it would call a good battery weak.
    if (rest_open_) {
        const uint32_t since = t_ms - started_ms_;
        if (running) {
            rest_open_ = false;
        } else if (since >= c.prime_ms) {
            const float avg = rest_n_ ? rest_sum_ / static_cast<float>(rest_n_) : vin_v;
            if (rest_n_ && vin_v < avg - kLoadDropV) {
                rest_open_ = false;
            } else {
                rest_sum_ += vin_v;
                ++rest_n_;
                if (since >= c.prime_ms + c.rest_window_ms) {
                    out_.rest_v = rest_sum_ / static_cast<float>(rest_n_);
                    rest_open_ = false;
                }
            }
        }
    }

    if (running && rpm >= c.charge_rpm) {
        if (!charging_) {
            charging_ = true;
            charge_since_ms_ = t_ms;
        } else if (t_ms - charge_since_ms_ >= c.charge_settle_ms) {
            out_.charge_v = smooth(out_.charge_v, vin_v);
        }
    } else {
        charging_ = false;
    }

    judge(c);
}

void Monitor::judge(const Config& c) {
    // The charging system first: a battery that is not being charged will
    // read weak soon enough, and the stator is the thing to go and look at.
    if (out_.charge_v > 0.0f && out_.charge_v < c.charge_low_v) {
        out_.verdict = Verdict::NotCharging;
    } else if (out_.charge_v > c.charge_high_v) {
        out_.verdict = Verdict::Overcharging;
    } else if ((out_.rest_v > 0.0f && out_.rest_v < c.weak_rest_v) ||
               (out_.crank_min_v > 0.0f && out_.crank_min_v < c.weak_crank_v)) {
        out_.verdict = Verdict::Weak;
    } else if (out_.rest_v > 0.0f || out_.crank_min_v > 0.0f || out_.charge_v > 0.0f) {
        out_.verdict = Verdict::Good;
    } else {
        out_.verdict = Verdict::Unknown;
    }
}

}  // namespace core::battery
