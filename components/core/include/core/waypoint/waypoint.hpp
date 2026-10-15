// The named waypoints and the rules for them (CRUISE -> NAV).
//
// A short fixed list rather than free naming: with five buttons and gloves
// on, picking "CAMP" from a list is one press, typing it is not. START is
// the way back to where the ride began, and it sets itself: at the first
// good fix of a ride, unless the START it already has is from this ride.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::waypoint {

enum Index : uint8_t { Start = 0, Truck, Camp, Fuel, Mark };
inline constexpr size_t kCount = 5;
static_assert(kCount == state::Waypoints::kCount, "names and slots disagree");

const char* name(size_t i);

// A GPS calendar day as one number: compare two, never do arithmetic on
// one. 0 is "not known".
uint32_t gps_day(uint16_t year, uint16_t day_of_year);

// Whether START should move to the current fix. A START from an earlier
// day, or more than `stale_ms` ago today, is an old ride's; one from this
// ride stays, so turning the key off for lunch does not move "home" to the
// lunch spot.
inline constexpr uint32_t kStartStaleMs = 6u * 3600u * 1000u;
bool start_is_stale(const state::Waypoint& start, uint32_t day, uint32_t utc_ms,
                    uint32_t stale_ms = kStartStaleMs);

// A fix good enough to remember: a fix, and no wilder than this HDOP.
inline constexpr float kMaxHdop = 3.0f;
bool good_fix(const state::Gnss& g);

// NVS holds doubles, so a waypoint is three of them: lat, lon, and `when`,
// the day and time together. A cleared waypoint stores `when` < 0.
double encode_when(const state::Waypoint& w);
void decode(double lat, double lon, double when, state::Waypoint& out);

}  // namespace core::waypoint
