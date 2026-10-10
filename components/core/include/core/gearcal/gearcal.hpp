// Calibrating the gear table: a guided run, and a very patient learner.
//
// `core/gear` identifies a gear by rpm / road speed against a table of
// ratios. The table depends on the sprockets and the rolling radius, so it
// has to be measured on the bike. Two things here do that:
//
//   Guide    the rider rides each gear at a steady speed and revs, and is
//            told when ten seconds of it have been seen. The measured ratio
//            and its percentage error against the table come out the far
//            end, to be accepted or thrown away.
//
//   Learner  runs all the time, notes how far each gear's ratio is from the
//            table on every clean stretch of riding, and, only after a lot
//            of agreeing evidence, nudges one entry by at most 1 %.
//
// Both rest on `Window`: a stretch of riding where rpm, road speed and the
// ratio between them hardly move, GPS agrees with the wheel, and the engine
// is in gear. Steadiness is the whole defence against wheelspin, clutch
// slip and GPS lag. Slip moves the ratio while rpm and speed are not
// following each other; lag matters only while speed is changing. A window
// that insists on neither moving cannot be fooled by either for long.
//
// Road speed here is the number `gear::estimate` is given at run time, not
// a better one. The table is a table of that ratio.
//
// Pure logic. Time comes in as a parameter, like everything in core/.
#pragma once
#include <cstdint>
#include "core/gear/gear.hpp"

namespace core::gearcal {

inline constexpr uint8_t kMaxGears = core::gear::kMaxGears;

// One tick of what the bike is doing.
struct Sample {
    uint32_t t_ms     = 0;
    float    rpm      = 0.0f;
    float    road_mph = 0.0f;     // what the gear estimate divides by
    float    gps_mph  = 0.0f;     // Doppler
    bool     gps_ok   = false;
    float    hdop     = 99.0f;
    bool     neutral  = false;
};

// Why a stretch of riding is not (yet) counting. `Steady` means nothing is
// wrong, not that ten seconds are in.
enum class Reason : uint8_t {
    Steady = 0,
    Neutral,       // in neutral, or the clutch is in far enough to say so
    Revs,          // engine too slow to be pulling anything
    Slow,          // road speed too low for a ratio to mean much
    NoGps,         // no fix, or a poor one
    GpsApart,      // the wheel and GPS disagree: slip, or a wrong circ.
    Unsteady,      // rpm, speed or their ratio moved: hold them still
    WrongGear,     // the ratio looks like a different gear
};
const char* reason_text(Reason r);

struct SteadyConfig {
    float min_rpm  = 2000.0f;
    float min_mph  = 6.0f;
    float max_hdop = 3.0f;
    // |wheel - GPS| as a fraction of GPS, on a half-second average.
    float gps_agree = 0.06f;
    // How far a quantity may wander within a window, (max - min) / mean,
    // judged on half-second averages so the wheel sensor's pulse
    // quantisation at crawling speed does not count as unsteadiness.
    float rpm_band   = 0.03f;
    float speed_band = 0.04f;
    float ratio_band = 0.03f;
    uint32_t block_ms = 500;
    // A gap in the samples longer than this (a stalled task, a breakpoint)
    // throws the window away instead of bridging it.
    uint32_t max_gap_ms = 1000;
};

class Window {
public:
    // Feed every tick. Returns why the stretch is not counting, or Steady.
    Reason feed(const Sample& s, const SteadyConfig& c);
    void   reset();

    uint32_t duration_ms() const { return blocks_ * block_ms_; }
    // Bumps every time the window is thrown away or restarted, so a caller
    // can tell "the same stretch, longer" from "a new one".
    uint32_t generation() const { return gen_; }

    float rpm() const      { return blocks_ ? sum_rpm_ / blocks_ : 0.0f; }
    float road_mph() const { return blocks_ ? sum_road_ / blocks_ : 0.0f; }
    float gps_mph() const  { return blocks_ ? sum_gps_ / blocks_ : 0.0f; }
    // rpm per mph, from the window's means; 0 with nothing in it.
    float ratio() const;
    // GPS over the wheel, minus one. Positive: the wheel reads low.
    float gps_over_road() const;

private:
    void restart_with(float rpm, float road, float gps);
    void clear_block();

    // The block being accumulated.
    uint32_t block_start_ms_ = 0;
    uint16_t block_n_ = 0;
    float    b_rpm_ = 0.0f, b_road_ = 0.0f, b_gps_ = 0.0f;
    // The window: a run of block means.
    uint32_t blocks_ = 0;
    uint32_t block_ms_ = 500;
    float    sum_rpm_ = 0.0f, sum_road_ = 0.0f, sum_gps_ = 0.0f;
    float    min_rpm_ = 0.0f, max_rpm_ = 0.0f;
    float    min_road_ = 0.0f, max_road_ = 0.0f;
    float    min_ratio_ = 0.0f, max_ratio_ = 0.0f;

    uint32_t last_t_ms_ = 0;
    bool     have_last_ = false;
    uint32_t gen_ = 1;
    Reason   reason_ = Reason::Steady;
};

// --- The guided run -----------------------------------------------------

enum class Phase : uint8_t {
    Idle = 0,
    Intro,        // the page that explains it and shows the learned errors
    Measuring,    // one gear at a time
    Summary,      // the results, before they are accepted
};

struct GuideConfig {
    SteadyConfig steady;
    // How long a gear must be held steady before the rider is told they
    // can move on. They may hold it longer; the longest stretch is kept.
    uint32_t min_ms = 10000;
    // A ratio further than this from the gear being asked for, and nearer
    // another gear, is the wrong gear rather than a wrong table.
    float wrong_gear_frac = 0.15f;
    // Having a result, a ratio this far below it for `shift_hold_ms` is the
    // rider changing up, and the run moves on by itself.
    float    shift_frac = 0.10f;
    uint32_t shift_hold_ms = 800;
    // Anything under this is not worth reporting as a wheel-size problem.
    float circ_report_frac = 0.01f;
};

class Guide {
public:
    explicit Guide(const GuideConfig& c = GuideConfig()) : cfg_(c) {}

    // Opens the intro page. `table` is the current rpm-per-mph table.
    void open(uint8_t gear_count, const float* table);
    void cancel();                    // back to Idle, results discarded

    // E, short. Intro: start. Measuring: move on, if this gear is ready.
    // Summary: accept. Returns true when a Summary was accepted, which is
    // the caller's cue to read the results out and write them into the
    // table; they stay readable until the next open().
    bool next();
    // E, long: leave this gear out.
    void skip();

    void step(const Sample& s);

    Phase    phase() const { return phase_; }
    uint8_t  gear() const { return gear_; }          // 1-based
    uint8_t  count() const { return count_; }
    bool     ready() const { return ready_; }
    Reason   reason() const { return reason_; }
    uint8_t  nearest() const { return nearest_; }    // with WrongGear
    uint32_t steady_ms() const { return win_.duration_ms(); }
    uint32_t need_ms() const { return cfg_.min_ms; }
    // The window as it stands, and how far it is from the table.
    float    live_ratio() const { return win_.ratio(); }
    float    live_err() const;                       // fraction, 0 with none

    uint8_t  done_mask() const { return done_; }
    bool     has_result(uint8_t gear) const;
    float    result_ratio(uint8_t gear) const;
    float    result_err(uint8_t gear) const;         // fraction vs the table
    // GPS over the wheel across every measured gear, as a fraction, when
    // it is big enough to be worth saying.
    bool     circ_valid() const { return circ_valid_; }
    float    circ_err() const { return circ_err_; }

private:
    void  enter_gear(uint8_t g);
    void  commit();                  // keep this gear's candidate
    void  advance();
    void  finish();

    GuideConfig cfg_;
    Phase    phase_ = Phase::Idle;
    uint8_t  count_ = 0;
    uint8_t  gear_ = 0;
    float    table_[kMaxGears] = {};

    Window   win_;
    Reason   reason_ = Reason::Steady;
    uint8_t  nearest_ = 0;
    bool     ready_ = false;
    uint32_t last_gen_ = 0;
    // The longest steady stretch seen in this gear.
    uint32_t cand_ms_ = 0;
    float    cand_ratio_ = 0.0f;
    float    cand_gps_ = 0.0f;
    uint32_t low_since_ms_ = 0;
    bool     low_ = false;

    uint8_t  done_ = 0;
    float    ratio_[kMaxGears] = {};
    float    gps_[kMaxGears] = {};
    bool     circ_valid_ = false;
    float    circ_err_ = 0.0f;
};

// --- The learner --------------------------------------------------------

struct LearnConfig {
    // Stricter than the guided run: the rider is not being asked to ride
    // carefully, so the stretches that count have to be unmistakable. GPS
    // within 2.5 % of the wheel also means a wrong wheel circumference
    // keeps the learner out rather than being "learned" into the table.
    SteadyConfig steady = [] {
        SteadyConfig c;
        c.min_rpm = 3000.0f;
        c.min_mph = 15.0f;
        c.max_hdop = 2.0f;
        c.gps_agree = 0.025f;
        return c;
    }();
    uint32_t min_ms = 6000;           // one stretch = one episode
    uint32_t min_gap_ms = 30000;      // between episodes in the same gear
    // An episode further than this from every gear is not evidence about
    // any of them; one too near a second gear is not unambiguous.
    float max_err = 0.08f;
    float neighbour_gap = 0.12f;
    // Agreeing episodes needed before anything moves, and what counts.
    static constexpr uint8_t kRing = 8;
    float deadband = 0.015f;          // everything seen must be this far off
    float max_spread = 0.04f;
    float max_step = 0.01f;           // per gear, per power cycle
    float max_drift = 0.05f;          // from the calibrated base, ever
};

class Learner {
public:
    explicit Learner(const LearnConfig& c = LearnConfig()) : cfg_(c) {}

    // Feed every tick while no guided run is open. Returns true when one
    // gear's entry should change, with the gear (1-based) and its new value;
    // the caller writes it into Settings. `base` is the table as last
    // calibrated or typed in, which bounds how far learning may ever move.
    bool step(const Sample& s, const float* table, const float* base,
              uint8_t count, uint8_t& gear, float& value);

    // Forget the evidence, e.g. after the table was edited by hand: the
    // errors on record were measured against numbers that are gone.
    void reset();
    void reset_gear(uint8_t gear);

    uint8_t episodes(uint8_t gear) const;
    // Median error of the episodes on record, as a fraction; 0 with none.
    float   error(uint8_t gear) const;
    bool    adjusted(uint8_t gear) const;
    uint8_t adjusted_mask() const;

private:
    LearnConfig cfg_;
    Window   win_;
    uint32_t seen_gen_ = 0;
    float    ring_[kMaxGears][LearnConfig::kRing] = {};
    uint8_t  n_[kMaxGears] = {};        // episodes on record (<= kRing)
    uint8_t  head_[kMaxGears] = {};
    uint32_t last_ms_[kMaxGears] = {};
    bool     have_last_[kMaxGears] = {};
    bool     adjusted_[kMaxGears] = {};
};

}  // namespace core::gearcal
