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
#include "core/gearcal/gearcal.hpp"
#include "core/lap/lap.hpp"
#include "core/ota/ota.hpp"
#include "core/wifi/wifi.hpp"

namespace core::state {

struct Engine {
    float rpm        = 0.0f;
    bool  neutral    = false;   // reads LOW on the wire; the driver inverts
    float coolant_c  = 0.0f;
    float tps_pct    = 0.0f;
    bool  ecu_online = false;   // false until the CAN adapter exists

    // The lamp pattern `ctrl` just sent to the PCA9955B, and whether it is
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
    // The IMU's die temperature, from its FIFO packets. Its gyro bias moves
    // with temperature, so it is worth seeing; it is also the nearest thing
    // to an inside-the-dash thermometer. 0 until the IMU has reported.
    float    imu_temp_c  = 0.0f;
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
    // Day of the year, 1..366, from the RMC date; 0 until a sentence has
    // carried one. utc_ms is time of day only, so this is what lets
    // core/sun know the season.
    uint16_t day_of_year = 0;
    // The same RMC date as a calendar date, for naming log files. 0 until
    // a sentence has carried one.
    uint16_t year  = 0;
    uint8_t  month = 0;
    uint8_t  day   = 0;
    // The system clock (ms since boot, the clock every task's `now` comes
    // from) when this fix's first sentence finished arriving. Moves once
    // per fix, not per sentence, so it is how `logic` tells a new fix from
    // the same one read again. Not the fix's time for the lap timer: that
    // is utc_ms put on this clock by core::fixtime, without the latency.
    uint32_t rx_ms = 0;
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

    // Splits (core::lap::Sectors), once the first clean lap has placed them.
    int8_t     sector            = -1;    // being ridden, 0..2; -1 none
    uint8_t    split_sector      = 0;     // the last split: which sector it ended
    uint32_t   split_ms          = 0;     // and its time; 0 before the first
    int32_t    split_delta_ms    = 0;     // against that sector's best
    bool       split_delta_valid = false;
    uint32_t   split_at_ms       = 0;     // when it was crossed, system clock
    uint32_t   ideal_ms          = 0;     // the three best sectors; 0 until all
    // The last lap: when it finished (system clock), and whether it was a
    // new session best, for LAP's flash.
    uint32_t   last_done_ms      = 0;
    bool       last_was_best     = false;
    // The session's deepest lean each way, above core::lap::Peaks::kMinMph.
    // Kept here, not in a widget: a screen is rebuilt every time it is
    // entered (1.17), and peaks kept in one were lost on leaving it.
    float      max_lean_left_deg  = 0.0f;
    float      max_lean_right_deg = 0.0f;
};

// One lap's peaks, for LAPS (core::lap::Peaks, as kept per lap by logic).
struct LapPeak {
    float mph = 0.0f;
    float rpm = 0.0f;
    float lean_deg = 0.0f;    // deeper of the two sides
};

Lap to_published(const lap::State& s, int32_t delta_ms = 0,
                 bool delta_valid = false);

// The P4 board senses VIN only (R17/R18 on GPIO20). There is no sense on
// the 5 V or 3V3 rails, so there are no fields for them: DIAG showed two
// rows that could never hold a number.
struct Elec {
    float vbat = 0.0f;    // VIN through the sense divider; 0 = no reading
};

// Battery and charging health, from core::battery: what this power-on has
// measured, 0 for not yet. `verdict` is a core::battery::Verdict.
struct Battery {
    float   rest_v      = 0.0f;
    float   crank_min_v = 0.0f;
    float   charge_v    = 0.0f;
    uint8_t verdict     = 0;
};

// Written by the `ble` task, from the phone's §11.1 Feed lines through
// core::media::apply_line(). Until a phone connects the fields stay at their
// defaults and the screens render nothing, which is also what they show for
// ten seconds after it leaves (core::media::kDisconnectHoldMs).
struct Media {
    char     title[60]  = {};
    char     album[60]  = {};
    char     artist[60] = {};
    // §11.1 tag `Y`. Streamed live from the phone's own .lrc reader, one
    // line at a time as playback reaches it — never a whole lyric file, and
    // never written to flash on this end. The phone owns the file and the
    // timing; the dash only ever holds the one line it was last told.
    char     lyric[60]  = {};
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

// §11's reserved "Ctrl" characteristic: what the rider last asked for, one
// CommandId::MediaCtrl payload at a time. `cmd` is 0xFF ("nothing posted
// yet") or 0/1/2 for previous/play-pause/next.
//
// This is a hand-off between `logic` (the one writer, via the command
// queue) and whichever task actually owns the radio, not a display value —
// no screen reads it, so it has no place in `Frame`. There is deliberately
// no sequence field here either: `Published<T>::generation()` already
// answers "is this a new one since I last looked" without a second number
// to keep in sync with the first.
struct MediaCtrl {
    uint8_t cmd = 0xFF;
};

// CONFIG's phone actions, handed from `logic` to `ble` the same way as
// MediaCtrl: not display data, and the generation says it is a new request.
struct BleRequest {
    enum Op : uint8_t { None = 0, ForgetPhone, PairingMode };
    uint8_t op = None;
};

// DIAG's Radio screen (§11.6). Unlike MediaCtrl, this *is* display data —
// what the link itself looks like, separate from what it's carrying — so
// it lives in Frame like Media and Tpms do.
struct BleDiag {
    int16_t rssi_dbm       = 0;
    bool    has_rssi       = false;   // false before any connection ever read one
    // The last §11.1 tag the dash accepted from the phone ('T','A','L','S',
    // 'D','P'), or 0 if nothing has arrived yet. A byte, not a whole line —
    // a title on a diagnostics table would just be the Media row again.
    char    last_tag       = 0;
    bool    ctrl_subscribed = false;  // phone is listening for Ctrl notifies
    bool    encrypted      = false;   // the current link, if any, is bonded and encrypted
    uint8_t bonds          = 0;       // phones the dash will let back in
    uint8_t pairing_s      = 0;       // seconds left in the pairing window; 0 closed
    // The C6's ESP-Hosted firmware and build id (core/c6), "2.12.8 #1a2b3c4d",
    // or "" before the link is up. "#-" is a build from before build ids.
    // DIAG's Update screen shows it beside the dash's own version.
    char    c6_fw[24]      = {};
};

// The two firmware slots and any update in progress (§11.7). Written by
// `ota`; the Update screen reads it.
struct Ota {
    bool             known = false;            // `ota` has published once
    core::ota::Role  role = core::ota::Role::Stable;
    char             running_ver[core::ota::kVersionLen] = {};
    char             running_slot[8] = {};     // "ota_0" / "ota_1"
    char             running_date[11] = {};    // "2026-10-08", from the image
    core::ota::Other other = core::ota::Other::Unknown;
    char             other_ver[core::ota::kVersionLen] = {};
    char             other_date[11] = {};
    core::ota::Install      install = core::ota::Install::Idle;
    uint8_t                 install_pct = 0;
    core::ota::InstallError error = core::ota::InstallError::None;
    core::ota::ImageError   image_error = core::ota::ImageError::None;
};

// CONFIG's card eject, handed from `logic` to `storage` like BleRequest.
struct StorageRequest {
    enum Op : uint8_t { None = 0, Eject };
    uint8_t op = None;
};

// The fused estimates (core/fusion), written by `logic`. Stage 1 is road
// speed only. The raw readings stay in Motion and Gnss, so DIAG can put the
// two side by side and the safety gates keep using what was measured.
struct Fused {
    float    speed_mph       = 0.0f;
    float    speed_sigma_mph = 0.0f;
    bool     speed_valid     = false;
    uint8_t  used            = 0;     // core::fusion::Source bits
    uint8_t  rejected        = 0;     // offered and gated out this step
    uint8_t  suspect         = 0;
    uint16_t resets          = 0;
    float    accel_bias_g    = 0.0f;
};

// The gear calibration's guided run and the learner's running errors, for
// the CONFIG calibration screen. Written by `logic`, which owns both.
// Errors are percent, signed: positive means the bike's measured ratio is
// above the table's, so the table is low.
struct GearCal {
    uint8_t  phase = 0;            // core::gearcal::Phase
    uint8_t  gear = 0;             // being measured, 1-based
    uint8_t  count = 0;
    uint8_t  reason = 0;           // core::gearcal::Reason
    uint8_t  nearest = 0;          // with WrongGear: the gear it looks like
    bool     ready = false;        // this gear has its steady seconds
    bool     autotune = false;
    uint16_t steady_ds = 0;        // steady for this long, tenths of a second
    uint16_t need_ds = 0;
    float    live_ratio = 0.0f;
    float    live_err_pct = 0.0f;
    uint8_t  done_mask = 0;        // gears measured in this run
    float    result_ratio[core::gearcal::kMaxGears] = {};
    float    result_err_pct[core::gearcal::kMaxGears] = {};
    bool     circ_valid = false;
    float    circ_err_pct = 0.0f;  // GPS over the wheel
    // The learner's evidence, per gear.
    uint8_t  learn_n[core::gearcal::kMaxGears] = {};
    float    learn_err_pct[core::gearcal::kMaxGears] = {};
    uint8_t  adjusted_mask = 0;    // gears it has moved this power cycle
};

// One place to get back to, for CRUISE -> NAV.
struct Waypoint {
    bool     set = false;
    double   lat = 0.0;
    double   lon = 0.0;
    uint32_t utc_ms = 0;     // when it was dropped, for "dropped 14:05"
    // Which GPS day it was dropped on (core::waypoint::gps_day), 0 if not
    // known. START uses it to tell today's ride from yesterday's.
    uint32_t day = 0;
};

// The named waypoints, in core::waypoint's order: START, which `logic` sets
// itself at the first good fix of a ride, then TRUCK, CAMP, FUEL and MARK
// for the rider to drop. Written by `logic`; `keeper` keeps them in NVS,
// so a lunch stop with the key off does not lose the way back.
struct Waypoints {
    static constexpr unsigned kCount = 5;
    Waypoint pts[kCount];
};

// The hotspot, for the Update screen. Written by `wifi`.
struct Wifi {
    core::wifi::State state = core::wifi::State::Off;
    char     pass[core::wifi::kPassLen + 1] = {};   // this session's, while on
    uint8_t  clients = 0;                            // phones/laptops joined
    uint16_t off_in_s = 0;                           // idle timer, while on
};

// Turn the hotspot on or off, from `logic` to `wifi` like BleRequest.
struct WifiRequest {
    enum Op : uint8_t { None = 0, Toggle, Off };
    uint8_t op = None;
};

// An upload from the page is on its way, from `wifi` to `ota`: this many
// bytes are about to arrive through Deps::ota_stream. `ota` stays the one
// writer of the dev slot; the web server only carries the bytes to it.
struct OtaUpload {
    uint32_t size = 0;
};

// The Update screen's actions, handed from `logic` to `ota` like BleRequest.
struct OtaRequest {
    enum Op : uint8_t { None = 0, InstallSd, BootDevOnce, Promote };
    uint8_t op = None;
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
    Imu = 0, Gnss, LedDriver, McpPod, Sd, Ecu, Display, BleHost, Phone,
    TpmsF, TpmsR, FrontWheel, RearWheel, Count
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
