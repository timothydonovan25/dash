// RPM to shift-lamp pattern. Pure logic.
//
// Eight lamps light progressively between base_rpm and base_rpm + span.
// Hysteresis stops a lamp flickering when RPM sits exactly on a threshold,
// which is what it does at a steady throttle.
#pragma once
#include <cstdint>

namespace core::shift {

inline constexpr int kLampCount = 8;

struct Config {
    uint16_t base_rpm       = 6000;   // lamp 1 lights here
    uint16_t span_rpm       = 2400;   // lamp 8 lights at base + span
    uint16_t hysteresis_rpm = 120;    // a lit lamp stays lit until this far below
    uint16_t flash_rpm      = 8800;   // above this, all lamps flash
    // Predictive lamps: decide them for where the engine will be this far
    // ahead, at the rate it is climbing now. The rider sees a lamp and shifts
    // a reaction time later, 150-250 ms, and in a low gear at full throttle
    // the rpm climbs thousands per second meanwhile, so a lamp on time is a
    // shift late. 0 is off: the lamps show the rpm as it is.
    uint16_t lead_ms        = 0;
    // The most the lead may add. A rate that is noise, or a glitch at the
    // ceiling of RateEstimator, can bring the lamps forward this far and
    // no further.
    uint16_t lead_cap_rpm   = 1000;
    // Rate below this is treated as noise and leads nothing; above it, only
    // the excess leads, so nothing jumps where the floor is crossed. A
    // steady engine with a percent of period jitter reads up to ~1000 rpm/s
    // either way (test_tach), which at a 150 ms lead would wobble a lamp by
    // more than the hysteresis. BRINGUP: measure the real tach's jitter.
    uint16_t lead_floor_rpm_s = 1000;
};

// The rpm the lamps are decided on: `rpm`, plus (rpm_per_s - the floor)
// times lead_ms while the engine is climbing, at most lead_cap_rpm more. A steady or
// falling engine is shown as it is -- predicting a fall would put lamps out
// early, which tells the rider nothing.
uint16_t lamp_rpm(float rpm, float rpm_per_s, const Config& c);

// Threshold for one lamp, 0-indexed.
uint16_t threshold(const Config& c, int lamp);

// Returns an 8-bit mask, bit 0 = lamp 1. `prev` is last call's mask and is
// what makes the hysteresis work — pass 0 on the first call.
uint8_t pattern(uint16_t rpm, const Config& c, uint8_t prev);

// True when RPM is past flash_rpm. The caller decides the blink rate, so
// this stays free of any notion of time.
bool over_limit(uint16_t rpm, const Config& c);

}  // namespace core::shift
