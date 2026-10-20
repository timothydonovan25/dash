// Performance timers: 0-30 and 0-60, 60 ft, 330 ft and the eighth mile from a
// stop, and 30-0 and 60-0 braking distance. What a Dragy box does, from the
// dash's own fused road speed (core::fusion), so there is nothing to mount.
//
// Pure logic, fed by `logic` every pass with the fused speed and published
// as the Perf slot for STAT -> PERF and the toast.
//
// How a run is timed:
//
//   - A launch is armed by standing still (under `stop_mph`) for
//     `armed_after_ms`, and starts when the speed comes up through
//     `stop_mph`. There is no rollout: the clock starts at the first
//     movement the speed estimate can see, which is a few hundredths later
//     than the wheel actually turning. Every run on this dash is timed the
//     same way, so runs compare with each other, if not with a drag strip.
//   - Each threshold (a speed, a distance) is crossed between two passes;
//     the time is interpolated to the crossing, so the 20 ms pass does not
//     become the resolution of the result.
//   - Distance is the integral of speed (trapezoids), not GPS position:
//     differencing fixes 0.1 s apart is noise at launch speeds.
//   - A launch ends when every threshold is done, when the speed falls
//     `abort_drop_mph` below the run's best (the rider rolled off), or
//     after `max_run_ms`. Thresholds not reached by then are not results.
//   - A braking run starts when the speed comes down through its start
//     speed (30 or 60) and ends at a stop. Coming back up past the start
//     speed abandons it. Both braking runs time at once: a stop from 70
//     gives a 60-0 and a 30-0.
//   - A speed estimate that goes invalid abandons whatever is under way.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::perf {

using Metric = state::Perf::Metric;
inline constexpr int kMetrics = state::Perf::kCount;

struct Config {
    float    stop_mph       = 0.5f;
    uint32_t armed_after_ms = 1000;
    // The speed runs' targets, and the braking runs' start speeds, in mph.
    // Metric riders get 50 and 100 km/h (`for_units`).
    float    to_mph[2]      = {30.0f, 60.0f};
    float    brake_mph[2]   = {30.0f, 60.0f};
    float    dist_ft[3]     = {60.0f, 330.0f, 660.0f};
    float    abort_drop_mph = 3.0f;
    // A braking run is abandoned if the speed comes back this far above
    // the lowest it reached: the rider eased off and rode on.
    float    brake_regain_mph = 2.0f;
    uint32_t max_run_ms     = 60000;
    // And if it has not reached a stop in this long. Without it, slowing
    // from 65 to 45 and riding on at 45 left the 60-0 run open for as long
    // as the ride lasted, and the next stop, minutes later, scored a
    // "braking distance" of miles. A 0.2 g stop from 60 takes 14 s.
    uint32_t max_brake_ms   = 15000;
};

// The thresholds a rider of these units expects: 0-30/0-60 mph, or
// 0-50/0-100 km/h (31.07 and 62.14 mph).
Config for_units(bool metric);

struct Input {
    uint32_t t_ms = 0;
    float    mph = 0.0f;
    bool     valid = false;
};

class Timer {
public:
    void update(const Input& in, const Config& c);
    // Forget the bests (the PERF screen's E). Runs and last results stay.
    void reset_bests();
    state::Perf publish(bool metric) const;

private:
    struct Brake {
        bool     on = false;
        uint32_t t0 = 0;
        float    ft = 0.0f;
        float    min_mph = 0.0f;   // the lowest this run has reached
    };

    void finish(int metric, uint32_t ms, float x);
    void end_launch();

    bool     have_prev_ = false;
    uint32_t prev_t_ = 0;
    float    prev_mph_ = 0.0f;

    uint32_t still_since_ = 0;     // when the speed last went under stop_mph
    bool     still_ = false;
    bool     armed_ = false;

    bool     launching_ = false;
    uint32_t launch_t0_ = 0;
    float    launch_ft_ = 0.0f;
    float    launch_peak_mph_ = 0.0f;
    bool     done_[kMetrics] = {};

    Brake    brake_[2];
    uint32_t live_ms_ = 0;

    state::Perf::Result last_[kMetrics];
    state::Perf::Result best_[kMetrics];
    uint16_t seq_ = 0;
    uint8_t  event_ = 0;
    bool     event_best_ = false;
};

}  // namespace core::perf
