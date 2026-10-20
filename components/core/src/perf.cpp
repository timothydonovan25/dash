#include "core/perf/perf.hpp"

namespace core::perf {
namespace {

constexpr float kFtPerSPerMph = 5280.0f / 3600.0f;
constexpr float kMphPerKmh = 0.621371f;

// Where between two samples a value crossed `level`, as a fraction 0..1 of
// the interval. The caller has already checked that it did.
float frac(float a, float b, float level) {
    const float d = b - a;
    if (d == 0.0f) return 1.0f;
    float f = (level - a) / d;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

}  // namespace

Config for_units(bool metric) {
    Config c;
    if (metric) {
        c.to_mph[0] = c.brake_mph[0] = 50.0f * kMphPerKmh;
        c.to_mph[1] = c.brake_mph[1] = 100.0f * kMphPerKmh;
    }
    return c;
}

void Timer::reset_bests() {
    for (auto& b : best_) b = state::Perf::Result{};
}

void Timer::finish(int metric, uint32_t ms, float x) {
    if (ms == 0) ms = 1;   // 0 means "not run"
    last_[metric] = state::Perf::Result{ms, x};
    // A distance run's best is its time; a braking run's is its distance.
    const bool brake = metric == Metric::Brake1 || metric == Metric::Brake2;
    state::Perf::Result& b = best_[metric];
    const bool better = b.ms == 0 || (brake ? x < b.x : ms < b.ms);
    if (better) b = last_[metric];
    ++seq_;
    if (seq_ == 0) seq_ = 1;
    event_ = static_cast<uint8_t>(metric);
    event_best_ = better;
}

void Timer::end_launch() {
    launching_ = false;
    live_ms_ = 0;
}

void Timer::update(const Input& in, const Config& c) {
    if (!in.valid) {
        // Nothing measured across a gap is a result.
        have_prev_ = false;
        still_ = armed_ = false;
        end_launch();
        brake_[0].on = brake_[1].on = false;
        return;
    }
    const float v = in.mph < 0.0f ? 0.0f : in.mph;
    if (!have_prev_) {
        have_prev_ = true;
        prev_t_ = in.t_ms;
        prev_mph_ = v;
        still_ = v < c.stop_mph;
        still_since_ = in.t_ms;
        return;
    }
    const uint32_t t0 = prev_t_;
    const float v0 = prev_mph_;
    const uint32_t dt = in.t_ms - t0;
    prev_t_ = in.t_ms;
    prev_mph_ = v;
    if (dt == 0) return;
    // The time at fraction f of this interval.
    const auto at = [&](float f) { return t0 + static_cast<uint32_t>(f * dt + 0.5f); };
    const float step_ft = (v0 + v) * 0.5f * kFtPerSPerMph * (dt / 1000.0f);

    // --- standing still arms a launch ---------------------------------
    if (v < c.stop_mph) {
        if (!still_) {
            still_ = true;
            still_since_ = in.t_ms;
        }
        if (in.t_ms - still_since_ >= c.armed_after_ms) armed_ = true;
    } else {
        still_ = false;
    }

    // --- launch -------------------------------------------------------
    if (armed_ && v0 < c.stop_mph && v >= c.stop_mph) {
        const float f = frac(v0, v, c.stop_mph);
        launching_ = true;
        armed_ = false;
        launch_t0_ = at(f);
        // Only the part of this interval after the start counts.
        launch_ft_ = (c.stop_mph + v) * 0.5f * kFtPerSPerMph * ((1.0f - f) * dt / 1000.0f);
        launch_peak_mph_ = v;
        for (bool& d : done_) d = false;
    } else if (launching_) {
        const float ft0 = launch_ft_;
        launch_ft_ += step_ft;
        if (v > launch_peak_mph_) launch_peak_mph_ = v;
        for (int i = 0; i < 2; ++i) {
            const int m = Metric::To1 + i;
            if (!done_[m] && v0 < c.to_mph[i] && v >= c.to_mph[i]) {
                done_[m] = true;
                finish(m, at(frac(v0, v, c.to_mph[i])) - launch_t0_, c.to_mph[i]);
            }
        }
        for (int i = 0; i < 3; ++i) {
            const int m = Metric::Ft60 + i;
            if (!done_[m] && ft0 < c.dist_ft[i] && launch_ft_ >= c.dist_ft[i]) {
                done_[m] = true;
                const float f = frac(ft0, launch_ft_, c.dist_ft[i]);
                finish(m, at(f) - launch_t0_, v0 + (v - v0) * f);
            }
        }
        bool all = true;
        for (int m = Metric::To1; m <= Metric::Eighth; ++m) all = all && done_[m];
        if (all || v < launch_peak_mph_ - c.abort_drop_mph ||
            in.t_ms - launch_t0_ > c.max_run_ms) {
            end_launch();
        }
    }

    // --- braking ------------------------------------------------------
    for (int i = 0; i < 2; ++i) {
        Brake& b = brake_[i];
        const float from = c.brake_mph[i];
        if (!b.on) {
            if (v0 >= from && v < from) {
                const float f = frac(v0, v, from);
                b.on = true;
                b.t0 = at(f);
                b.ft = (from + v) * 0.5f * kFtPerSPerMph * ((1.0f - f) * dt / 1000.0f);
                b.min_mph = v;
            }
            continue;
        }
        if (v < b.min_mph) b.min_mph = v;
        if (v > b.min_mph + c.brake_regain_mph || in.t_ms - b.t0 > c.max_brake_ms) {
            b.on = false;
            continue;
        }
        if (v < c.stop_mph) {
            // To the stop, and only the part of this interval before it.
            const float f = frac(v0, v, c.stop_mph);
            b.ft += (v0 + c.stop_mph) * 0.5f * kFtPerSPerMph * (f * dt / 1000.0f);
            b.on = false;
            finish(Metric::Brake1 + i, at(f) - b.t0, b.ft);
        } else {
            b.ft += step_ft;
        }
    }

    if (launching_) {
        live_ms_ = in.t_ms - launch_t0_;
    } else if (brake_[1].on || brake_[0].on) {
        const Brake& b = brake_[1].on ? brake_[1] : brake_[0];
        live_ms_ = in.t_ms - b.t0;
    } else {
        live_ms_ = 0;
    }
}

state::Perf Timer::publish(bool metric) const {
    state::Perf p;
    p.phase = launching_ ? 2 : (brake_[0].on || brake_[1].on) ? 3 : armed_ ? 1 : 0;
    p.live_ms = live_ms_;
    p.live_ft = launching_ ? launch_ft_
              : brake_[1].on ? brake_[1].ft
              : brake_[0].on ? brake_[0].ft : 0.0f;
    p.metric = metric;
    for (int i = 0; i < kMetrics; ++i) {
        p.last[i] = last_[i];
        p.best[i] = best_[i];
    }
    p.seq = seq_;
    p.event = event_;
    p.event_best = event_best_;
    return p;
}

}  // namespace core::perf
