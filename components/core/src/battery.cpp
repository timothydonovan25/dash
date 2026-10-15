#include "core/battery/battery.hpp"

namespace core::battery {

namespace {
// Rest and charge are smoothed: a reading is a level, not a sample.
constexpr float kAlpha = 0.05f;
// More than this below the resting average at once is something drawing
// current, not the battery settling.
constexpr float kLoadDropV = 0.5f;
// A starter turns for under a second or two before the engine catches.
constexpr uint32_t kLoadMs = 2000;
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

void Monitor::update(float vin_v, float rpm, uint32_t t_ms, const Config& c) {
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
        running_ = rpm > 0.0f;
        state_since_ms_ = t_ms;
        load_v_ = vin_v;
        last_ms_ = t_ms;
        bucket_[0] = bucket_[1] = vin_v;
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
    load_v_ += a * (vin_v - load_v_);

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
        state_since_ms_ = t_ms;
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

    // A sudden drop is a load -- the starter, before the first tach edge
    // says the engine is turning -- not the battery's resting level. One
    // that lasts is: a light left on, or a battery that has run down.
    if (!running && t_ms - state_since_ms_ >= c.rest_settle_ms) {
        const bool drop = out_.rest_v != 0.0f && vin_v < out_.rest_v - kLoadDropV;
        if (!drop) {
            dropped_ = false;
            out_.rest_v = smooth(out_.rest_v, vin_v);
        } else if (!dropped_) {
            dropped_ = true;
            drop_since_ms_ = t_ms;
        } else if (t_ms - drop_since_ms_ >= kLoadMs) {
            out_.rest_v = vin_v;        // it stayed down: that is the level now
            dropped_ = false;
        }
    } else {
        dropped_ = false;
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
