// Lap delta against the session best (§18, decided).
//
// ────────────────────────────────────────────────────────────────────────
//  PLACEHOLDER. Deliberately the simplest thing that is honest, so it can
//  be replaced without disturbing anything above it. What is here:
//
//    reference   the best clean lap of this session, in RAM, lost at power
//                off. No saved per-track reference, no track identity.
//    matching    nearest point on the reference, searched in a window
//                around the last match.
//    precision   one decimal, and the number is quantised to match. See
//                the note on kQuantumMs — this is not a display choice.
//
//  What it is not: tuned. Every threshold below is a guess that has never
//  seen a real GPS trace, and the tests around it are synthetic. Treat the
//  numbers as a starting point for the first track day, not as settled.
// ────────────────────────────────────────────────────────────────────────
//
// Why nearest-point rather than distance-along-lap: distance drifts. Take a
// wider line through one corner and you have travelled further, so at
// "1000 ft into the lap" you are not where the reference was at 1000 ft, and
// the error compounds for the rest of the lap. Matching on position is
// immune to that and costs a bounded search.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::delta {

// 2.5 minutes at 10 Hz. Longer than any lap this bike will do on a club
// circuit; a lap that overruns simply stops recording and cannot become the
// reference, which is the right failure for a wrong-sized buffer.
inline constexpr size_t kMaxPoints = 1536;

struct Config {
    // How far either side of the last match to look. Forward is generous
    // because a fix dropout can leave the anchor stale; backward is tight
    // because going backwards along a lap means the match was wrong.
    uint16_t search_back    = 15;
    uint16_t search_forward = 60;

    // Beyond this from the nearest reference point, the rider is not on the
    // reference line — off track, in the pits, or on a part of the circuit
    // the reference lap never covered. No delta is better than a wrong one.
    float max_match_ft = 120.0f;

    // The number is rounded to this before anyone sees it.
    //
    // Not a display preference. The relative error between two laps recorded
    // minutes apart does not fully cancel — slow multipath and ionospheric
    // drift stay in — so the honest resolution of this figure is a tenth or
    // two. Rounding at the source means no screen, no log and no future
    // reader can imply precision that was never there.
    uint32_t quantum_ms = 100;
};

struct Point {
    float    x_ft = 0.0f;       // local frame, same origin as the traces
    float    y_ft = 0.0f;
    uint32_t elapsed_ms = 0;    // since the crossing that started the lap
};

class Tracker {
public:
    // A crossing started a new lap. Discards the part-recorded lap.
    void start_lap();

    // One fix, at the 10 Hz the receiver produces. `elapsed_ms` is the lap
    // clock, which the lap timer already interpolates.
    void sample(float x_ft, float y_ft, uint32_t elapsed_ms, const Config& c);

    // The lap just ended. `is_best` promotes what was recorded to be the new
    // reference — the caller decides, because the lap timer already knows
    // whether the lap was clean and whether it beat the best.
    void finish_lap(bool is_best);

    // LapClear: forget the reference and the session.
    void clear();

    // A reference from elsewhere: a track's saved best lap, already moved
    // into this power-on's frame (core::track::shift). Replaces whatever
    // reference there was; the lap being recorded carries on. Fewer than
    // two points is no reference.
    void load_reference(const Point* pts, size_t n);

    // The reference itself, for saving it with the track.
    const Point* reference() const { return ref_; }

    bool   has_reference() const { return ref_count_ > 1; }
    size_t reference_points() const { return ref_count_; }
    size_t recorded_points() const { return rec_count_; }

    // False when there is no reference, no match, or the match was too far
    // away to believe. The caller publishes the flag beside the number so a
    // widget can draw `--` rather than a confident zero.
    bool delta_ms(int32_t& out) const;

    // Diagnostics, for the DIAG screens during bring-up.
    float  last_match_ft() const { return last_match_ft_; }
    size_t last_match_index() const { return match_; }

private:
    Point  rec_[kMaxPoints];
    size_t rec_count_ = 0;
    Point  ref_[kMaxPoints];
    size_t ref_count_ = 0;

    size_t  match_ = 0;
    bool    anchored_ = false;      // false until the first match of a lap
    // A lap has just started and nothing has matched yet: the rider is at
    // the line, so the first search is the reference's start, not all of it.
    bool    at_start_ = false;
    bool    valid_ = false;
    int32_t delta_ms_ = 0;
    float   last_match_ft_ = 0.0f;
};

}  // namespace core::delta
