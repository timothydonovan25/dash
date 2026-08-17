// Airtime from the accelerometer (§8). The one thing on a dirt bike that a
// 6-axis IMU measures without any argument.
//
// In free fall an accelerometer reads zero — not "down", not 1 g, zero,
// because it measures specific force and a falling body has none. So the
// detector is a threshold and two timers, and unlike lean it needs no
// filter, no GPS and no trust.
//
// The 100 ms qualifying window is what separates a jump from a whoop. A
// bike unloading over a bump reads under 0.3 g for 30-60 ms; the shortest
// jump a rider would count is a couple of hundred.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::jump {

struct Config {
    float    free_fall_g      = 0.30f;   // below this, possibly airborne
    uint32_t min_free_fall_ms = 100;     // ...for this long, definitely
    float    landing_g        = 2.00f;   // above this, back on the ground
    // The impact peak arrives after the threshold crossing, not on it — the
    // suspension is still compressing. Sampling the trigger reads maybe 2 g
    // on a landing that actually hit 5. Keep watching for this long, then
    // report the real peak.
    uint32_t impact_window_ms = 150;
    uint32_t max_airtime_ms   = 8000;    // longer than this is a dropped sensor
};

enum class Event : uint8_t { None = 0, TookOff, Landed };

struct Update {
    Event    event       = Event::None;
    uint32_t airtime_ms  = 0;     // set on Landed
    float    landing_g   = 0.0f;  // set on Landed: the peak, not the trigger
};

class Detector {
public:
    // `mag_g` is the magnitude of the acceleration vector, not one axis. A
    // bike in the air can be at any angle and often is.
    Update update(float mag_g, uint32_t t_ms, const Config& c);

    void reset();

    bool     airborne() const   { return airborne_; }
    // Counts up live while in the air, so the screen can show it climbing.
    uint32_t airtime_ms() const { return airtime_ms_; }
    uint32_t best_ms() const    { return best_ms_; }
    float    last_landing_g() const { return last_landing_g_; }

    void publish_into(state::Attitude& a) const;

private:
    bool     airborne_    = false;
    bool     falling_     = false;   // under the threshold, not yet qualified
    bool     impacting_   = false;   // down, still watching for the peak
    bool     suspended_   = false;   // sensor is talking nonsense; wait it out
    uint32_t fall_start_  = 0;
    uint32_t impact_start_= 0;
    uint32_t landed_airtime_ms_ = 0;
    uint32_t landed_at_ms_ = 0;      // survives reset(), like the peak g does
    uint32_t airtime_ms_  = 0;
    uint32_t best_ms_     = 0;
    float    peak_g_      = 0.0f;
    float    last_landing_g_ = 0.0f;
};

}  // namespace core::jump
