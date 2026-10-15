// Battery and charging health, from VIN and rpm alone.
//
// The CRF250F starts on its battery, and the way a rider finds out it is
// tired, or that the stator or regulator has quit charging it, is a dead
// starter at the trailhead. VIN sense (R17/R18) and the tach already tell
// three things about that, and this watches for them:
//
//   rest      the battery with the engine off, settled: state of charge
//   crank     the lowest VIN while the starter turns: how much it has left
//             under load, which falls off well before the rest voltage does
//   charge    VIN held above a steady rpm: whether the charging system is
//             charging, and not boiling the battery
//
// Pure logic, fed by `power` at 100 Hz with the lowest VIN of each 10 ms
// (the crank dip lasts a few hundred ms; a 1 Hz reading never sees it).
// Below `bike_min_v` the board is on USB, and nothing is learned.
//
// The thresholds are for a 12 V lead-acid / AGM battery. A lithium one
// rests higher and dips less, so it reads Good on the same numbers.
#pragma once
#include <cstdint>

namespace core::battery {

struct Config {
    float    bike_min_v       = 8.0f;     // core::power's bike/USB line
    uint32_t rest_settle_ms   = 10000;    // engine off this long first
    float    charge_rpm       = 3000.0f;
    uint32_t charge_settle_ms = 3000;     // above charge_rpm this long first
    uint32_t crank_window_ms  = 4000;     // after the first rpm is seen
    // The crank dip is read through this much smoothing. The 9.6 V line is
    // for the voltage the battery holds while the starter turns, not for
    // the first few tens of milliseconds of the motor's inrush, which dip a
    // healthy battery well below it: read raw, nearly every start was weak.
    uint32_t crank_avg_ms     = 100;
    float    weak_rest_v      = 12.2f;    // about half charged
    float    weak_crank_v     = 9.6f;     // the usual load-test line
    float    charge_low_v     = 13.2f;
    float    charge_high_v    = 15.0f;
};

enum class Verdict : uint8_t {
    Unknown = 0,     // nothing measured yet this power-on
    Good,
    Weak,            // low at rest, or sags too far cranking
    NotCharging,
    Overcharging,
};

const char* verdict_name(Verdict v);

// What has been measured this power-on. 0 means not yet.
struct Report {
    float   rest_v      = 0.0f;
    float   crank_min_v = 0.0f;
    float   charge_v    = 0.0f;
    Verdict verdict     = Verdict::Unknown;
};

class Monitor {
public:
    void update(float vin_v, float rpm, uint32_t t_ms, const Config& c);
    const Report& report() const { return out_; }

private:
    void judge(const Config& c);

    Report   out_;
    bool     started_ = false;
    bool     running_ = false;
    uint32_t state_since_ms_ = 0;     // when the engine last went on or off
    // VIN smoothed over crank_avg_ms, and the time of the last sample.
    float    load_v_ = 0.0f;
    uint32_t last_ms_ = 0;
    // The lowest of that over the last ~second, in two half-second buckets,
    // so the dip from a starter that turns before the first tach edge counts.
    float    bucket_[2] = {0.0f, 0.0f};
    uint32_t bucket_ms_ = 0;
    bool     cranking_ = false;
    uint32_t crank_start_ms_ = 0;
    float    crank_min_ = 0.0f;
    uint32_t charge_since_ms_ = 0;
    bool     charging_ = false;
    bool     dropped_ = false;        // VIN just fell well below rest
    uint32_t drop_since_ms_ = 0;
};

}  // namespace core::battery
