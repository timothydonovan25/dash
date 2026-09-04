// The published vehicle state. Plain data, no behaviour.
//
// Every struct here is trivially copyable because each one lives in a
// Published<T> slot (§4.1) and is copied whole by readers. Keep them that
// way: no std::string, no pointers to anything a reader might outlive.
//
// Units are in the field names, always. `_mph`, `_ft`, `_deg`, `_g`, `_ms`,
// `_c`, `_psi`. This is the rule that stops a degrees/radians bug.
#pragma once
#include <cstdint>
#include "core/lap/lap.hpp"

namespace core::state {

struct Engine {
    float rpm        = 0.0f;
    bool  neutral    = false;   // reads LOW on the wire; the driver inverts
    float coolant_c  = 0.0f;
    float tps_pct    = 0.0f;
    bool  ecu_online = false;   // false until the CAN adapter exists

    // The lamp pattern `ctrl` just sent to the MCP23S17, and whether it is
    // over the limit. Published rather than recomputed by the screen: the
    // pattern carries hysteresis state, so a screen working it out again
    // from `rpm` would keep its own and could show something different from
    // the lamps actually lit on the handlebar.
    uint8_t shift_mask  = 0;
    bool    shift_flash = false;

    // Inferred from rpm against road speed — there is no gear sensor on this
    // bike. 0 means the estimate is not to be trusted right now: clutch in,
    // stopped, or a ratio that matches nothing. Neutral is separate, above,
    // because that one really is sensed.
    uint8_t gear = 0;
};

struct Motion {
    float gps_mph   = 0.0f;
    float front_mph = 0.0f;
    float rear_mph  = 0.0f;
    float best_mph  = 0.0f;     // best of the three, for the big readout
    float accel_g   = 0.0f;
};

struct Attitude {
    float    lean_deg    = 0.0f;
    float    pitch_deg   = 0.0f;
    float    yaw_rate_dps= 0.0f;
    float    g_lat       = 0.0f;
    float    g_lon       = 0.0f;
    float    g_vert      = 0.0f;
    bool     airborne    = false;
    uint32_t airtime_ms  = 0;
    float    last_land_g = 0.0f;
    // When the last landing was reported. A screen that wants to show the
    // jump for a few seconds and then go back to what it was showing cannot
    // work this out for itself: `airtime_ms` holds the figure indefinitely,
    // and a Frame is one instant with no history in it. 0 means no jump yet.
    uint32_t land_ms     = 0;
};

struct Gnss {
    bool     fix        = false;
    uint8_t  sats       = 0;
    uint8_t  quality    = 0;
    double   lat        = 0.0;
    double   lon        = 0.0;
    float    alt_m      = 0.0f;
    float    hdop       = 0.0f;
    float    course_deg = 0.0f;
    float    speed_mph  = 0.0f;   // Doppler, not differenced positions
    uint32_t utc_ms     = 0;
};

struct Trip {
    float odo_mi        = 0.0f;
    float trip_a_mi     = 0.0f;
    float trip_b_mi     = 0.0f;
    float fuel_used_gal = 0.0f;
    float eng_hours     = 0.0f;
    float top_mph       = 0.0f;
};

// The lap timer's own state plus the delta, which the timer does not own:
// the delta needs a reference trace, and that belongs to core/delta.
struct Lap {
    lap::Phase phase           = lap::Phase::NoLine;
    uint16_t   count           = 0;
    uint32_t   current_ms      = 0;
    uint32_t   last_ms         = 0;
    uint32_t   best_ms         = 0;
    int32_t    delta_ms        = 0;
    // False when there is no reference lap yet, or the rider is off the
    // reference line. A delta of 0 means "dead on your best lap", which is
    // a very different statement from "no idea", so the two must not share
    // a representation.
    bool       delta_valid     = false;
    float      dist_to_line_ft = 0.0f;
};

Lap to_published(const lap::State& s, int32_t delta_ms = 0,
                 bool delta_valid = false);

struct Elec {
    float vbat = 0.0f;    // no VIN sense this revision; see dash-v1 review §3
    float v5   = 0.0f;
    float v3v3 = 0.0f;
};

// Written by the `ble` task, which is not in this build. The fields stay at
// their defaults and the screens render `--`, which is exactly what they do
// for a device that is Absent. Keeping the shape here means adding the radio
// later does not disturb the Frame contract.
struct Media {
    char     title[60]  = {};
    char     album[60]  = {};
    char     artist[60] = {};
    uint8_t  state      = 0;
    uint8_t  src        = 0;
    // Zero means nothing has ever arrived, and every screen treats that as
    // "no media" rather than as an empty song. When the phone goes away the
    // `ble` task must clear this slot: a dashboard still showing what was
    // playing forty minutes ago is worse than one showing nothing.
    uint32_t updated_ms = 0;

    // §11.1 tags `P`/`D`. The phone does not write `position_ms` on every
    // tick of its own playback clock — that would be a BLE write a second
    // for a field the dash can extrapolate itself — so this is an anchor:
    // "the phone said `position_ms` was true at dash-clock `position_at_ms`".
    // A screen wanting the live elapsed time adds `frame.t_ms -
    // position_at_ms` to `position_ms` while `state == 1` (playing), and
    // shows `position_ms` unchanged otherwise. `duration_ms` of 0 means the
    // phone hasn't said, same "unknown, not zero" convention as everywhere
    // else here.
    uint32_t position_ms    = 0;
    uint32_t duration_ms    = 0;
    uint32_t position_at_ms = 0;
};

struct TpmsWheel {
    float    psi     = 0.0f;
    float    temp_c  = 0.0f;
    uint8_t  batt_pct= 0;
    bool     alarm   = false;
    uint32_t age_ms  = 0;
    bool     valid   = false;
};

struct Tpms {
    TpmsWheel front;
    TpmsWheel rear;
};

// §12. Absent means "not fitted or never seen"; Fault means the keeper
// should retry it. A widget bound to anything but Ok shows `--`.
enum class DevState : uint8_t { Absent = 0, Init, Ok, Degraded, Fault };

struct DevHealth {
    DevState state       = DevState::Absent;
    uint16_t fault_count = 0;
    uint32_t last_ok_ms  = 0;
    int32_t  last_err    = 0;
};

// The architecture sketches Health as named fields. It is an indexed table
// here instead, because the DIAG sensors screen renders it as a table and
// walking named members would mean writing that list out twice.
enum class Dev : uint8_t {
    Imu = 0, Gnss, McpBoard, McpPod, Sd, Ecu, Display, BleHost, Phone,
    TpmsF, TpmsR, Count
};
inline constexpr uint8_t kDevCount = static_cast<uint8_t>(Dev::Count);

const char* dev_name(Dev d);

struct Health {
    DevHealth dev[kDevCount] = {};

    const DevHealth& operator[](Dev d) const {
        return dev[static_cast<uint8_t>(d)];
    }
    DevHealth& operator[](Dev d) { return dev[static_cast<uint8_t>(d)]; }

    // The question every widget asks before it shows a number.
    bool ok(Dev d) const { return (*this)[d].state == DevState::Ok; }
};

}  // namespace core::state
