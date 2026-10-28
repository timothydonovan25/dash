// The detail log: what the first rides are for.
//
// Nothing on this dash has run on the bike. The blackbox (log.hpp) records
// what the rider was shown; this records what each of the assumptions behind
// those numbers rests on, so a ride can say which of them were wrong. It sits
// beside the blackbox and leaves it alone: the RaceRender CSV, the replay
// tool, the GPX export and the Laps page all keep working on it unchanged.
//
// Five more files a session, each from one producer through its own ring
// (Ring is single-producer), named after the session's blackbox file:
//
//   detail/<stem>_imu.csv   imu,    200 Hz  every raw sample, sensor axes,
//                                           before the mount rotation and
//                                           the bias: lean math, mount,
//                                           vibration, the stunt and jump
//                                           thresholds can all be re-run.
//   detail/<stem>_tach.csv  ctrl,   50 Hz   the tach and wheel pulses as the
//                                           ISR saw them: intervals, edges
//                                           the refractory window threw away
//                                           and how late after the spark,
//                                           extra and missed pulses, edge-to-
//                                           lamp latency, ctrl's work time.
//   detail/<stem>_det.csv   logic,  50 Hz   what the filters made of it:
//                                           every speed source and fusion's
//                                           verdict on each, the attitude
//                                           filter's insides, stunt lift,
//                                           gear and its ratio.
//   detail/<stem>_evt.csv   logic,  event   one line per thing that happened:
//                                           every command, health changes,
//                                           fixes, laps, jumps, tricks, gear
//                                           changes, power cuts, drops.
//   detail/<stem>_diag.csv  logger, 1 Hz    the machine: heap, stacks, timing
//                                           overruns, temperatures, battery,
//                                           every device's state, each
//                                           stream's rows and drops, and the
//                                           card's own write times.
//
// `<stem>` is the blackbox's name without `.csv`, renamed with it when the
// GPS gives the date. Under detail/ rather than logs/ so the Wi-Fi page,
// which serves logs/, does not offer a 40 MB IMU file as a ride to analyse.
// tools/ridelog reads them off the card.
//
// CONFIG -> Log -> Detail log turns all five off. On by default until the
// bike has been proven.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/log/log.hpp"
#include "core/state/command.hpp"
#include "core/state/vehicle.hpp"

namespace core::log {

// --- streams -----------------------------------------------------------------

enum Stream : uint8_t {
    kMain = 0,   // the blackbox, logs/<stem>.csv
    kImu,
    kTach,
    kDet,
    kEvt,
    kDiag,
    kStreamCount
};

// "imu", "tach", ...; "" for kMain.
const char* stream_suffix(Stream s);

// "detail/s007_imu.csv" before the date is known, then
// "detail/2026-10-13_s007_imu.csv". kMain gives log_file_name()'s name.
// Returns the length, or 0 if it did not fit.
size_t stream_file_name(char* buf, size_t n, Date date, unsigned session, Stream s);

inline constexpr const char* kDetailDir = "detail";

// --- imu: 200 Hz raw ----------------------------------------------------------

// One FIFO sample as the driver gave it: sensor axes, before the mount
// rotation and the gyro bias, so every step of the attitude filter can be
// re-run from it on a laptop.
struct ImuRow {
    uint32_t t_ms   = 0;
    float    ax_g   = 0.0f, ay_g = 0.0f, az_g = 0.0f;
    float    gx_dps = 0.0f, gy_dps = 0.0f, gz_dps = 0.0f;
    float    temp_c = 0.0f;
    // On the first sample of each FIFO read, how many came in it, 0 on the
    // rest; and which read of this wake it was. A wake that needed several
    // reads found the FIFO backed up: SPI2 held by the card, or a flash
    // write freezing core 0. That is the shared-bus assumption, measured.
    uint8_t  batch_n = 0;
    uint8_t  drain   = 0;
    // The part's own timestamp and the FIFO packet's header, raw
    // (hal::IImu::Sample). t_ms is the P4's back-dated guess; the chip's
    // clock, differenced row to row (it wraps every 65.5 ms at 1 us), is
    // what the part actually did. tools/ridelog compares the two.
    uint16_t chip_ts  = 0;
    uint8_t  fifo_hdr = 0;
};

size_t imu_header(char* buf, size_t n);
size_t format_imu(char* buf, size_t n, const ImuRow& r);

// --- tach and wheels: 50 Hz, from ctrl -----------------------------------------

// The pulses one channel counted in a window, and what their spacing says.
struct PulseStats {
    uint16_t pulses = 0;
    // Reads whose count went up by more than one: the intervals were then
    // averaged over several pulses and min/max cannot see inside them.
    uint16_t multi  = 0;
    // Reads whose count rose with no newer edge time (core::tach's torn
    // read). The pulses stay uncounted until a good edge.
    uint16_t torn   = 0;
    // An interval under half the one before it: an extra edge got past the
    // refractory window (spark ringing, a second magnet, a noisy Hall). Over
    // 1.8 times it: a pulse never arrived (R34/C19 ate the spark, a magnet
    // fell off). Steady running should show neither.
    uint16_t short_n = 0;
    uint16_t long_n  = 0;
    // Per-pulse interval, microseconds, over the window. 0 with none.
    uint32_t min_us = 0;
    uint32_t max_us = 0;
    uint32_t last_us = 0;
};

// Builds PulseStats from a free-running counter and its last edge time, the
// same pair core::tach::Meter reads. ctrl feeds every read; take() hands the
// window over and starts the next.
class PulseWindow {
public:
    // An interval longer than this is a restart (engine stopped, wheel
    // still), not a slow pulse: counted, but kept out of the statistics.
    static constexpr uint32_t kMaxIntervalUs = 1000000;

    void       update(uint32_t total, uint64_t edge_us);
    PulseStats take();
    const PulseStats& peek() const { return w_; }

private:
    PulseStats w_;
    uint32_t   last_total_ = 0;
    uint64_t   last_edge_us_ = 0;
    uint32_t   prev_us_ = 0;
    bool       started_ = false;
};

struct TachRow {
    uint32_t   t_ms = 0;
    PulseStats tach, front, rear;
    // Edges the ISR's refractory window threw away this window, and the
    // latest of them after the edge it kept, in us. The window is the one
    // number the tach input lives on (drivers/engine_sense.hpp): ringing that
    // reaches near its end is a second count waiting to happen.
    uint32_t   tach_rejected = 0;
    uint32_t   tach_ring_us  = 0;
    uint16_t   front_rejected = 0;
    uint16_t   rear_rejected  = 0;
    float      raw_rpm = 0.0f;     // unsmoothed, what the lamps go by
    float      rpm = 0.0f;         // smoothed, what the needle shows
    float      rpm_rate = 0.0f;    // rpm/s
    uint16_t   lamp_rpm = 0;       // after the lead and the sweep
    uint8_t    mask = 0;           // lamps lit (acknowledged)
    bool       neutral = false;
    bool       sense_ok = false;
    float      front_mph = 0.0f;
    float      rear_mph = 0.0f;
    // Timing. The budget is tach edge to lamp under 3 ms.
    uint16_t   edge_wakes = 0;       // ctrl woken by a tach edge
    uint32_t   wake_lat_max_us = 0;  // edge to ctrl reading it
    uint32_t   lamp_lat_max_us = 0;  // edge to its lamp write done
    uint32_t   work_max_us = 0;      // longest tick body
    uint16_t   lamp_writes = 0;
    uint16_t   lamp_errors = 0;
    uint16_t   late = 0;             // ctrl's late ticks, running total
};

size_t tach_header(char* buf, size_t n);
size_t format_tach(char* buf, size_t n, const TachRow& r);

// --- derived state: 50 Hz, from logic --------------------------------------------

struct DetRow {
    uint32_t t_ms = 0;
    // Speed: every source, and fusion's verdict on each (core::fusion).
    float    shown_mph = 0.0f;       // Motion::best_mph, the speedo
    float    front_mph = 0.0f, rear_mph = 0.0f, gps_mph = 0.0f;
    float    hdop = 0.0f;
    bool     gps_new = false;
    float    fused_mph = 0.0f, fused_sigma = 0.0f;
    bool     fused_valid = false;
    uint8_t  used = 0, rejected = 0, suspect = 0;   // core::fusion::Source bits
    uint16_t resets = 0;
    float    accel_g = 0.0f;          // what fusion was given
    float    accel_bias_g = 0.0f;
    uint8_t  front_verdict = 0, rear_verdict = 0;  // core::fusion::Wheel
    // Attitude (core::attitude::Filter).
    float    lean_deg = 0.0f, accel_lean_deg = 0.0f, pitch_deg = 0.0f;
    float    steer_deg = 0.0f, yaw_rate_dps = 0.0f;
    float    slide_rate_dps = 0.0f, heading_deg = 0.0f;
    bool     sliding = false, airborne = false;
    float    g_lat = 0.0f, g_lon = 0.0f, g_vert = 0.0f;
    float    bias_gx = 0.0f, bias_gy = 0.0f, bias_gz = 0.0f;
    // Stunt: the ground's angle and the bike's against it (core::stunt).
    float    ground_deg = 0.0f, lift_deg = 0.0f;
    uint8_t  stunt_kind = 0;
    // Engine and gear. `ratio` is rpm per mph, what core::gear matches
    // against the table; 0 below 3 mph where it means nothing.
    float    rpm = 0.0f;
    float    ratio = 0.0f;
    uint8_t  gear = 0;
    bool     neutral = false;
    uint8_t  shift_mask = 0;
    // The rest.
    uint8_t  lap_phase = 0;
    float    dist_to_line_ft = 0.0f;
    uint8_t  perf_phase = 0;
    uint8_t  gearcal_phase = 0;
    float    vbat = 0.0f;
    bool     still = false;           // core::flash::still
};

size_t det_header(char* buf, size_t n);
size_t format_det(char* buf, size_t n, const DetRow& r);

// --- events ----------------------------------------------------------------------

enum class Ev : uint8_t {
    None = 0,
    Command,      // a: CommandId; i: its int, or the ConfigSet key; x: ConfigSet value
    Health,       // a: Dev; b: DevState; i: last_err; x: fault_count
    Fix,          // a: 1 gained, 0 lost; b: sats; x: hdop
    Lap,          // b: laps; i: lap ms; a: 1 a new best
    Split,        // a: sector ended; i: its ms
    LapPhase,     // a: core::lap::Phase
    TakeOff,
    Land,         // i: airtime ms; x: landing g
    Slide,        // a: 1 start, 0 end; x: slide rate dps
    Stunt,        // a: kind; b: grade; i: ms; x: peak deg; y: points
    Perf,         // a: metric; b: 1 best; i: ms; x: trap mph or ft
    Wheel,        // a: 0 front, 1 rear; b: core::fusion::Wheel
    PowerFail,    // a: 1 cut, 0 back
    Battery,      // a: verdict; x: rest V; y: crank V
    Gear,         // a: gear; x: rpm; y: mph
    Engine,       // a: 1 running, 0 stopped; x: rpm
    Moving,       // a: 1 moving, 0 still (core::flash::still)
    GearCal,      // a: core::gearcal::Phase
    FusionReset,  // i: resets so far
    Suspect,      // a: core::fusion::Source bits now suspect
    Drop,         // a: Stream; i: rows dropped so far
    Heart,        // a: core::state::Heart::Link; b: bpm then (0 none)
    HrRecovery,   // a: 1 with the two-minute fall; b: bpm at the stop; i: fall at 1 min; x: at 2
};

struct EventRow {
    uint32_t t_ms = 0;
    Ev       kind = Ev::None;
    uint8_t  a = 0;
    uint16_t b = 0;
    int32_t  i = 0;
    float    x = 0.0f;
    float    y = 0.0f;
};

EventRow command_event(uint32_t t_ms, const state::Command& c);

// "Time,Event,Detail": the detail is key=value pairs, a value with a space
// in it quoted ("LED driver"), so the line reads as it stands and splits
// with shlex.
size_t event_header(char* buf, size_t n);
size_t format_event(char* buf, size_t n, const EventRow& e);

// What the watcher compares from one logic pass to the next. Filled by
// `logic` from the slots it has just read and written.
struct Watched {
    state::Health health;
    bool     fix = false;
    uint8_t  sats = 0;
    float    hdop = 0.0f;
    uint16_t lap_count = 0;
    uint32_t last_lap_ms = 0;
    uint32_t last_done_ms = 0;
    bool     last_was_best = false;
    uint32_t split_at_ms = 0;
    uint8_t  split_sector = 0;
    uint32_t split_ms = 0;
    uint8_t  lap_phase = 0;
    bool     airborne = false;
    uint32_t land_ms = 0;
    uint32_t airtime_ms = 0;
    float    land_g = 0.0f;
    bool     sliding = false;
    float    slide_rate_dps = 0.0f;
    uint16_t stunt_seq = 0;
    uint8_t  stunt_kind = 0, stunt_grade = 0;
    uint32_t stunt_ms = 0;
    float    stunt_peak_deg = 0.0f;
    uint32_t stunt_points = 0;
    uint16_t perf_seq = 0;
    uint8_t  perf_event = 0;
    bool     perf_best = false;
    uint32_t perf_ms = 0;
    float    perf_x = 0.0f;
    uint8_t  front_verdict = 0, rear_verdict = 0;
    bool     power_fail = false;
    uint8_t  battery_verdict = 0;
    float    rest_v = 0.0f, crank_v = 0.0f;
    uint8_t  gear = 0;
    float    rpm = 0.0f, mph = 0.0f;
    bool     moving = false;
    uint8_t  gearcal_phase = 0;
    uint16_t fusion_resets = 0;
    uint8_t  suspect = 0;
    uint32_t dropped[kStreamCount] = {};
    uint8_t  hr_link = 0;          // core::state::Heart::Link
    uint16_t hr_bpm = 0;
    bool     hr_no_pulse = false;  // core::hr::no_pulse
    uint16_t hrr_seq = 0;          // core::state::HrRecovery
    uint16_t hrr_from_bpm = 0;
    int16_t  hrr_drop1 = 0;
    int16_t  hrr_drop2 = 0;
    bool     hrr_has2 = false;
};

// Turns one pass's Watched into the events since the last. The first call
// compares against a default Watched, so whatever is already true at
// key-on (devices up, a fix) is logged once as it starts.
class EventWatcher {
public:
    static constexpr size_t kMaxPerPass = 32;
    size_t watch(const Watched& now, uint32_t t_ms, EventRow* out, size_t max);

private:
    Watched prev_;
};

// --- diag: 1 Hz, from logger -------------------------------------------------------

struct DiagRow {
    uint32_t t_ms = 0;
    uint32_t heap_free_b = 0, heap_min_b = 0, psram_free_b = 0;
    char     stack_low_task[12] = {};
    uint32_t stack_low_free_b = 0;
    uint16_t ctrl_late = 0;
    uint16_t render_hz_x10 = 0;
    uint32_t imu_timeouts = 0;
    uint32_t gnss_sentences = 0, gnss_bad_csum = 0;
    uint16_t pod_recoveries = 0, pod_stuck = 0;
    float    chip_temp_c = 0.0f;
    bool     chip_temp_ok = false;
    float    imu_temp_c = 0.0f;
    float    vbat = 0.0f;
    float    rest_v = 0.0f, crank_v = 0.0f, charge_v = 0.0f;
    uint8_t  battery_verdict = 0;
    bool     fix = false;
    uint8_t  sats = 0, quality = 0;
    float    hdop = 0.0f;
    float    odo_mi = 0.0f, eng_hours = 0.0f, fuel_used_gal = 0.0f;
    // Rows each stream has written and dropped, and the card: bytes this
    // second, its slowest write or sync this second, writes over 100 ms,
    // and failures. The SD-stall budget behind every ring's size is a
    // guess; this is the measurement.
    uint32_t rows[kStreamCount] = {};
    uint32_t dropped[kStreamCount] = {};
    uint32_t sd_bytes = 0;
    uint32_t sd_max_ms = 0;
    uint16_t sd_slow = 0;
    uint16_t sd_errors = 0;
    uint8_t  dev_state[state::kDevCount] = {};
    uint16_t dev_faults[state::kDevCount] = {};
    int16_t  rssi_dbm = 0;
    uint8_t  wifi_state = 0;
};

size_t diag_header(char* buf, size_t n);
size_t format_diag(char* buf, size_t n, const DiagRow& r);

// --- rings -------------------------------------------------------------------------
// Ten seconds or more of slack each against a stalled card, like the
// blackbox's. In PSRAM (main/wiring.cpp).
using ImuRing  = Ring<ImuRow, 2048>;    // 10.2 s at 200 Hz
using TachRing = Ring<TachRow, 512>;    // 10.2 s at 50 Hz
using DetRing  = Ring<DetRow, 512>;
using EvtRing  = Ring<EventRow, 256>;

}  // namespace core::log
