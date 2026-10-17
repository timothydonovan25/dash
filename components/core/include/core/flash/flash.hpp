// May a task write the P4's flash right now?
//
// A flash write freezes core 0 for its whole length -- up to ~20 ms when NVS
// erases a page -- because IDF suspends the other core's cache and the P4
// has no flash auto-suspend (IDF v6.0). `ctrl` cannot run through that, so
// the shift lamps would stall mid-pull. Every flash writer that is not
// strictly parked (OTA is: core::ota::may_write) asks this first: `keeper`'s
// odometer, hours, settings and waypoints, and NimBLE's bond store.
//
// "Still" is deliberately pessimistic about speed. It takes the highest of
// every source rather than the one the speedo shows, because the speedo's
// source can read 0 while the bike moves: a dead wheel sensor that has not
// been faulted yet, or a wheel nobody fitted a magnet to. A sensor that
// reads too high only delays a save; one that reads too low costs a lamp.
//
// The engine may idle. A setting changed in the garage must not wait for
// key-off, and at idle the lamps are dark. Revving is not still: a rider
// holding the throttle on a start gate is watching the lamps.
#pragma once
#include <cstdint>

namespace core::flash {

struct Stillness {
    float    front_mph = 0.0f;
    float    rear_mph  = 0.0f;
    float    gps_mph   = 0.0f;
    bool     gps_fix   = false;
    float    rpm       = 0.0f;
    // The rpm the first shift lamp lights at (ShiftView::base_rpm).
    uint16_t lamp_base_rpm = 0;
};

// Wheel speed below this is stopped; the same line OTA uses.
inline constexpr float kStillWheelMph = 0.5f;
// GPS Doppler wanders by a mph or so on a parked bike under trees, so its
// line is wider. It only has to catch a bike that is clearly moving.
inline constexpr float kStillGpsMph = 3.0f;

bool still(const Stillness& s);

}  // namespace core::flash
