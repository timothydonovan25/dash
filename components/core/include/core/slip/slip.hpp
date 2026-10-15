// Wheelspin and lockup, from the two wheel sensors against road speed.
//
// Spin is the rear turning faster than the bike is moving: power on, in
// sand, mud or off a loose climb. Lockup is a wheel turning slower than the
// bike is moving, under braking: the front tucking, or the rear skidding.
// Both are read against road speed, not against each other, because the
// two wheels can be wrong together -- a rear skid with the front braking.
// Road speed is the fused figure (core::fusion), which already refuses a
// spinning or locked wheel as a source, with GPS Doppler behind it.
//
// Pure logic. ui::App runs it on every Frame and draws the result as a bar
// across the top of the screen, so it reads at a glance mid-corner.
#pragma once
#include <cstdint>

namespace core::slip {

enum class Kind : uint8_t { None = 0, Spin, LockFront, LockRear };

const char* kind_name(Kind k);

struct Config {
    // Below this the wheels' pulse counts are too coarse to compare, and a
    // stop is not a lockup.
    float    min_mph      = 6.0f;
    // Slip, as a fraction of road speed, before it is shown. A knobby on
    // dirt slips a few percent driving forward all the time.
    float    spin_start   = 0.12f;
    float    lock_start   = 0.25f;
    // A shown slip is held this long after it ends, so a half-second spin
    // is on the glass long enough to be read rather than a flicker.
    uint32_t hold_ms      = 700;
};

struct Reading {
    Kind    kind = Kind::None;
    // How much, in whole percent of road speed (for a lock, how far below
    // it the wheel is; 100 is stopped dead). Quantised to 5 so a slip that
    // wanders by a percent is not a new picture every frame.
    uint8_t pct  = 0;

    bool operator==(const Reading& o) const { return kind == o.kind && pct == o.pct; }
    bool operator!=(const Reading& o) const { return !(*this == o); }
};

class Meter {
public:
    // `road_mph` with `road_ok` false (no fusion, no GPS) leaves only the
    // wheels against each other: spin is still seen, a lock is not.
    //
    // A wheel with `_ok` false is a sensor the firmware has already called
    // dead (core::fusion::WheelCheck): it reads 0, which is exactly what a
    // locked wheel reads, so it is left out. Without this a dead front was
    // a red FRONT LOCK 100 % for as long as it stayed dead.
    Reading update(float front_mph, bool front_ok, float rear_mph, bool rear_ok,
                   float road_mph, bool road_ok, uint32_t t_ms, const Config& c);

private:
    Reading  held_;
    uint32_t held_until_ms_ = 0;
};

}  // namespace core::slip
