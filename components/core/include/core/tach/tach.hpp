// RPM and wheel speed from edge counts. Pure maths.
//
// The `ctrl` task reads free-running pulse counters at 500 Hz and turns them
// into numbers here. Two things this has to get right, both of which are
// how a tach normally goes wrong:
//
//  - **Time between edges, not between polls.** At 500 Hz and 6000 rpm the
//    pulses are 10 ms apart, so four ticks in five see nothing. Dividing by
//    the poll interval instead of by the edge interval reads 30000 rpm.
//    That is why the driver timestamps edges in its ISR and why `update()`
//    takes that timestamp rather than working it out from the counter.
//  - **Zero has to mean zero.** With no pulses, an average over the last
//    window decays but never arrives, and the dash reads 400 rpm on a bike
//    that has been switched off for a minute. There is an explicit stall
//    timeout for that.
//  - **The counter wraps.** A uint32 of pulses at 6000 rpm wraps in about
//    eight days of running, and every difference below is an unsigned
//    subtraction so that it does not matter.
#pragma once
#include <cstdint>

namespace core::tach {

struct Config {
    uint8_t  pulses_per_rev = 1;
    // For an ignition that sparks only every other revolution: each pulse
    // is this many revolutions. Settings::tach_revs_per_pulse.
    uint8_t  revs_per_pulse = 1;

    // No pulse for this long and the engine is not running. One revolution
    // at 300 rpm takes 200 ms, so 500 ms is comfortably below any idle.
    uint32_t stall_ms = 500;

    // Exponential smoothing on the output, 0..1. Higher follows faster. The
    // shift lights read the unsmoothed value; this is for the display, where
    // a needle that twitches is just noise.
    float    smoothing = 0.35f;
};

struct WheelConfig {
    uint16_t circumference_mm = 2130;
    uint8_t  pulses_per_rev   = 6;      // spoke magnets, or sprocket bolts
    uint32_t stall_ms         = 1500;   // a wheel can legitimately be slow
    // Smoothing as a time constant, not a fraction per pulse. Per pulse, the
    // lag grew as the wheel slowed: ~1.5 pulse intervals, 240 ms at 5 mph
    // and 20 ms at 60. In time, it is about this long at any speed.
    uint32_t tau_ms           = 50;
};

// One conversion, exposed because it is the thing worth testing on its own.
float rpm_from_pulses(uint32_t pulses, uint32_t dt_us, uint8_t pulses_per_rev);
float mph_from_pulses(uint32_t pulses, uint32_t dt_us, const WheelConfig& c);

class Meter {
public:
    // `total` is the free-running counter and `edge_us` is when its most
    // recent pulse arrived, both from one latched read (hal::IEngineSense).
    // `now_us` is the poll time, and is only used to decide when silence has
    // gone on long enough to mean the engine has stopped.
    void update(uint32_t total, uint64_t edge_us, uint64_t now_us,
                const Config& c);

    float rpm() const { return rpm_; }
    float raw_rpm() const { return raw_rpm_; }   // unsmoothed, for the lamps
    bool  running() const { return running_; }
    void  reset();

private:
    uint32_t last_total_ = 0;
    uint64_t last_edge_us_ = 0;
    float    rpm_ = 0.0f;
    float    raw_rpm_ = 0.0f;
    bool     running_ = false;
    bool     started_ = false;
};

// Between pulses the reading cannot be higher than one pulse's distance
// over the time since the last edge (with 10% slack for uneven magnet
// spacing), so a wheel that is slowing reads slower as soon as its next
// pulse is late, rather than holding its last speed until the next one or
// until `stall_ms`. That bound shapes the output only; the smoothed speed
// underneath is untouched, so the next pulse picks up where it left off.
class WheelMeter {
public:
    void update(uint32_t total, uint64_t edge_us, uint64_t now_us,
                const WheelConfig& c);
    float mph() const { return out_; }
    void  reset();

private:
    uint32_t last_total_ = 0;
    uint64_t last_edge_us_ = 0;
    float    mph_ = 0.0f;      // smoothed, updated on pulses
    float    out_ = 0.0f;      // mph_, bounded by the time since the last edge
    bool     started_ = false;
};

// Which of the three speeds to believe. GPS is accurate but late and dies
// under trees; the wheels are instant but wrong when they are spinning or
// locked. Front wheel first — a driven rear wheel lies about speed under
// power, and this is a dirt bike.
float best_speed_mph(float front_mph, bool front_ok,
                     float rear_mph,  bool rear_ok,
                     float gps_mph,   bool gps_ok);

}  // namespace core::tach
