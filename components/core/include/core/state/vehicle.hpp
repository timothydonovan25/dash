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
    // No coolant temperature: the CRF250F is air-cooled.
    float tps_pct    = 0.0f;
    bool  ecu_online = false;   // false until the CAN adapter exists

    // The lamp pattern `ctrl` just sent to the PCA9955B, and whether it is
    // over the limit. Published rather than recomputed by the screen: the
    // pattern carries hysteresis state, so a screen working it out again
    // from `rpm` would keep its own and could show something different from
    // the lamps actually lit on the handlebar.
    uint8_t shift_mask  = 0;
    bool    shift_flash = false;
    // The rpm reached before the last shift, held half a second
    // (core::shift::PeakHold), and the lamp that keeps it lit on the
    // handlebar. 0 for none: nothing is held while the engine is climbing
    // with the needle on its own peak. The lamp is apart from shift_mask, so
    // RACE's bar, coloured by the lamps the rpm lights, does not stay amber
    // through every hold; `ctrl` sends the two together.
    float   rpm_peak    = 0.0f;
    uint8_t shift_hold  = 0;

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

    // The filter's insides, for the detail log (core/log/detail.hpp): what
    // each lean figure rests on. The steering angle it took out (bar-mounted
    // IMU), what the accelerometer alone says, the gyro heading and the slide
    // check against the GPS course, and the gyro bias it is using, in the
    // sensor's axes. No screen reads them.
    float    steer_deg      = 0.0f;
    float    accel_lean_deg = 0.0f;
    float    heading_deg    = 0.0f;
    float    slide_rate_dps = 0.0f;
    bool     sliding        = false;
    float    bias_gx_dps    = 0.0f;
    float    bias_gy_dps    = 0.0f;
    float    bias_gz_dps    = 0.0f;
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

    // The saved track this line belongs to (core::track), -1 for none: no
    // line, no card, or a line not yet saved. A track that came back by
    // itself is `track_recalled`, which LAP says, so a rider who did not set
    // a line knows why one is there.
    int8_t     track             = -1;
    bool       track_recalled    = false;
    // Moves each time a Set line was refused for want of speed: the line's
    // direction is the GPS course, which means nothing at a standstill.
    // The toast says why (ui::App), as it does for a new best.
    uint16_t   line_refused_seq  = 0;
    // Its record and its ideal lap over every visit; 0 for none yet.
    uint32_t   track_best_ms     = 0;
    uint32_t   track_ideal_ms    = 0;
};

// The ride so far, for TRACK -> SESSION (core::session). Written by `logic`.
struct Session {
    float    dist_mi        = 0.0f;
    uint32_t moving_s       = 0;     // above core::session::kMovingMph
    uint32_t engine_s       = 0;
    float    top_mph        = 0.0f;
    float    avg_mph        = 0.0f;  // over moving time
    uint32_t best_air_ms    = 0;     // longest jump, 0 none
    float    hardest_land_g = 0.0f;
    uint16_t jumps          = 0;
};

// Performance timers (core::perf), for STAT -> PERF. Times in ms, distances
// in ft, speeds in mph whatever the display units: `metric` only says which
// thresholds the speed runs were timed to (0-50/0-100 km/h instead of
// 0-30/0-60 mph), so the screen can label them.
struct Perf {
    // The runs, in this order. Speed runs from a stop, distance runs from a
    // stop with the speed at the line (trap), braking runs to a stop.
    enum Metric : uint8_t {
        To1 = 0,    // 0-30 mph / 0-50 km/h
        To2,        // 0-60 mph / 0-100 km/h
        Ft60,       // 60 ft
        Ft330,      // 330 ft
        Eighth,     // 1/8 mile, 660 ft
        Brake1,     // 30-0 mph / 50-0 km/h
        Brake2,     // 60-0 mph / 100-0 km/h
        kCount
    };
    struct Result {
        uint32_t ms = 0;      // 0: not run yet
        float    x = 0.0f;    // trap mph for a distance run, ft for a brake run
    };
    // What the timer is doing: 0 idle (moving, or not stopped long enough),
    // 1 armed (stopped, ready to launch), 2 launching, 3 braking.
    uint8_t  phase   = 0;
    uint32_t live_ms = 0;     // elapsed in the run under way
    float    live_ft = 0.0f;
    bool     metric  = false;
    Result   last[kCount];
    Result   best[kCount];
    // The newest result, for the toast: which, and whether it was a best.
    // `seq` moves once per result, 0 until the first.
    uint16_t seq        = 0;
    uint8_t  event      = 0;   // a Metric
    bool     event_best = false;
};

// Wheelies and stoppies, scored (core::stunt), for STAT -> STUNT and the
// toast. Angles are the bike's pitch against the ground it is riding on,
// not against level: a hill is not a wheelie.
struct Stunt {
    // The trick under way: 0 none, 1 wheelie, 2 stoppie.
    uint8_t  live_kind   = 0;
    uint32_t live_ms     = 0;
    float    live_deg    = 0.0f;   // now, + nose up
    float    live_points = 0.0f;   // before the combo multiplier
    bool     live_balance = false; // in the balance-point band right now
    // The combo: tricks chained within core::stunt::kComboWindowMs of each
    // other. `combo_x10` is the multiplier the next trick will get, x10.
    uint8_t  combo       = 0;
    uint8_t  combo_x10   = 10;
    // The last trick, finished and scored. `seq` moves once per trick.
    uint16_t seq         = 0;
    uint8_t  kind        = 0;      // 1 wheelie, 2 stoppie, 3 looped out
    uint8_t  grade       = 0;      // core::stunt::grade_name()
    uint8_t  landing     = 0;      // 0 plain, 1 smooth, 2 slammed
    uint32_t ms          = 0;
    float    peak_deg    = 0.0f;
    uint32_t points      = 0;      // with the multiplier
    uint8_t  mult_x10    = 10;
    bool     record      = false;  // the session's best single trick
    // When it was scored (dash clock): SPEED shows it for a few seconds,
    // as it does a jump from Attitude::land_ms. 0 until the first.
    uint32_t done_ms     = 0;
    // The session.
    uint32_t total       = 0;
    uint16_t tricks      = 0;
    uint32_t best_points = 0;
    uint32_t longest_ms  = 0;
    float    highest_deg = 0.0f;
    uint8_t  best_combo  = 0;
};

// The phone's last notification (§11.1 tag `N`), for the toast and CRUISE ->
// PHONE. One, not a list: the Frame is copied every render pass.
struct Notify {
    // 'C' incoming call (ringing), 'M' message, 'X' missed call, 'O' other.
    // A call stops ringing on tag `N` kind 'E' or when the phone goes away.
    char     kind = 0;
    bool     ringing = false;
    char     who[32] = {};      // caller, sender, or the app's name
    char     text[96] = {};     // the message; empty for a call
    uint32_t at_ms = 0;         // when it arrived, dash clock
    uint16_t seq = 0;           // moves once per notification, 0 until one
};

// Heart rate from the rider's watch (core/hr), over the standard Heart Rate
// Service. Written by `ble`. `bpm` is the last reading and `at_ms` when it
// came; core::hr::fresh says whether it is still worth showing.
struct Heart {
    enum Link : uint8_t {
        Off = 0,      // CONFIG -> Heart rate -> Watch HR is off
        Searching,    // scanning, or waiting to scan again
        Connecting,   // connecting, finding the service, subscribing
        Live,         // subscribed: readings arrive
    };
    uint8_t  link = Off;
    uint16_t bpm = 0;            // 0 when the last reading had no pulse in it
    uint32_t at_ms = 0;          // dash clock of the last reading; 0 none yet
    bool     worn = false;       // a pulse seen since this link came up
    bool     contact_known = false;
    bool     contact = false;
    int8_t   rssi_dbm = 0;       // 0 before one was read
    char     name[20] = {};      // the sensor's advertised name, if any
    // This power-on, from every reading.
    uint16_t avg_bpm = 0;
    uint16_t max_bpm = 0;
    uint16_t min_bpm = 0;
    uint32_t readings = 0;
    uint16_t seq = 0;            // moves once per reading
    // CONFIG -> Heart rate -> Max HR, carried here so STAT -> HEART can draw
    // zones from the Frame alone.
    uint16_t zone_max_bpm = 0;
};

// Heart-rate recovery (core::hr::Recovery), written by `logic`: how far the
// pulse falls in the first minute (and second) of being parked after a ride.
struct HrRecovery {
    bool     measuring = false;   // parked, the minute running
    uint32_t start_ms = 0;        // when the bike stopped
    uint16_t start_bpm = 0;       // the pulse then
    int16_t  live_drop = 0;       // start_bpm less the pulse now, while measuring
    // The last finished measurement: the fall at one minute, and at two if
    // the bike stayed parked that long (has_drop2).
    int16_t  drop1 = 0;
    int16_t  drop2 = 0;
    uint16_t from_bpm = 0;        // its start_bpm
    bool     has_drop2 = false;
    int16_t  best_drop1 = 0;      // this power-on
    uint16_t count = 0;           // one-minute results this power-on
    uint16_t seq = 0;             // moves with each result (one minute or two)
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

// CONFIG's Calibrate level, handed from `logic` to `imu` like BleRequest:
// the samples it needs are only in `imu`. Its answer comes back as two
// ConfigSets for the mount offset, so Settings keeps its one writer.
struct ImuRequest {
    enum Op : uint8_t { None = 0, CalibrateLevel };
    uint8_t op = None;
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
    // Working, but short of what it should do (core::health's limited):
    // the GPS stuck at 1 Hz. It is why `state` reads Degraded; the data is
    // still good, which is what working() is for.
    bool     limited     = false;
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

    // Delivering good data, if not all of it: Ok, or Degraded only for
    // being limited (a 1 Hz GPS). For a reading that is right whatever the
    // rate; a fault still blanks it.
    bool working(Dev d) const {
        const DevHealth& h = (*this)[d];
        return h.state == DevState::Ok || (h.state == DevState::Degraded && h.limited);
    }
};

}  // namespace core::state
