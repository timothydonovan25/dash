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
};

// Threshold for one lamp, 0-indexed.
uint16_t threshold(const Config& c, int lamp);

// Returns an 8-bit mask, bit 0 = lamp 1. `prev` is last call's mask and is
// what makes the hysteresis work — pass 0 on the first call.
uint8_t pattern(uint16_t rpm, const Config& c, uint8_t prev);

// True when RPM is past flash_rpm. The caller decides the blink rate, so
// this stays free of any notion of time.
bool over_limit(uint16_t rpm, const Config& c);

}  // namespace core::shift
