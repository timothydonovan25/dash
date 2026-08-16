// Odometer, trip meters, engine hours, fuel estimate. Pure logic.
//
// The published `Trip` struct is floats, because that is what gets drawn.
// The accumulator underneath is not, and that distinction is the whole
// reason this class exists:
//
//   float odo = 100000.0f;  odo += 0.0001f;   // odo is still 100000.0f
//
// A float carries about 7 significant digits. Past 100,000 miles the gap
// between representable values is bigger than the distance covered in one
// 20 ms tick at 50 mph, so every increment rounds away and the odometer
// stops moving. It is a slow, silent, unrecoverable failure, and it is why
// the totals below are doubles and are only narrowed on the way out.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::trip {

struct FuelModel {
    // There is no fuel-flow signal on this bike and no ECU yet, so burn is
    // modelled from throttle position between two rates. These are a guess
    // for a CRF250F and exist to be corrected against a tank of fuel and
    // the odometer, not to be trusted.
    float idle_gph = 0.18f;
    float wot_gph  = 1.40f;
    float tank_gal = 1.60f;

    float gph_at(float tps_pct) const;
};

struct Input {
    float    speed_mph  = 0.0f;
    bool     engine_on  = false;
    float    tps_pct    = 0.0f;
    uint32_t t_ms       = 0;
};

class Computer {
public:
    // Restored from NVS at boot. The `life` namespace holds these two and
    // nothing else may write them.
    void restore(double odo_mi, double engine_hours);

    // Call every logic tick. Time between calls is derived from `t_ms`, so
    // a missed tick costs distance rather than inventing it. The first call
    // only establishes the timebase.
    void update(const Input& in, const FuelModel& fm);

    void reset_trip_a();
    void reset_trip_b();
    void reset_fuel();

    state::Trip publish() const;

    // For the keeper task, which writes NVS at 1 Hz but only when this says
    // something changed — flash endurance, not laziness.
    double odo_mi() const { return odo_mi_; }
    double engine_hours() const { return engine_hours_; }

private:
    double odo_mi_        = 0.0;
    double trip_a_mi_     = 0.0;
    double trip_b_mi_     = 0.0;
    double engine_hours_  = 0.0;
    double fuel_used_gal_ = 0.0;
    float  top_mph_       = 0.0f;

    uint32_t last_ms_ = 0;
    bool     started_ = false;
};

}  // namespace core::trip
