// Split times, the ideal lap, and each lap's peaks. Pure logic, beside the
// lap Timer and fed the same fixes.
//
// Splits place themselves. Two lines, at a third and two thirds of the
// distance round the first clean lap, each across the track at the GPS
// course there -- nothing for the rider to set, and they work on any track,
// which a track database would not (§18 has no track identity yet). A line
// is the finish line's kind (core::geo::crosses_line, the same half width,
// the same interpolation inside the fix pair) and the splits must be passed
// in order, so a piece of track that runs beside another cannot trip the
// wrong one. Setting or clearing the finish line forgets them.
//
// Each sector's best is kept; the ideal lap is the three bests added up. A
// sector the fix dropped out of is shown but is never a best, as the Timer
// treats a gapped lap.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/geo/geo.hpp"
#include "core/lap/lap.hpp"

namespace core::lap {

inline constexpr int kSectors = 3;

// What a split crossing says.
struct Split {
    bool     crossed  = false;
    uint8_t  sector   = 0;       // 0..2, the sector it ended
    uint32_t ms       = 0;       // that sector's time
    // Against the best of the same sector before this one; `delta_valid`
    // false when there was none, or either was gapped.
    int32_t  delta_ms = 0;
    bool     delta_valid = false;
    uint32_t at_ms    = 0;       // when, on the fixes' clock
};

class Sectors {
public:
    // Once per fix, after Timer::update() with the same fix, and that call's
    // result. `course_deg` is the GPS course with the fix, for placing lines.
    Split update(const Fix& f, float course_deg, const Update& lap, const Config& c);

    // The finish line was set or cleared: the splits were for that line.
    void reset();
    // LapArm: the lap in progress is thrown away, lines and bests kept.
    void abort_lap();

    bool     placed() const { return placed_; }
    // The sector being ridden, while one is being timed; -1 otherwise.
    int      current() const { return timing_ ? current_ : -1; }
    uint32_t best_ms(int k) const { return best_[k]; }
    // The three bests added up; 0 until every sector has one.
    uint32_t ideal_ms() const;

    // Where the lines went, and the course across each: what a saved track
    // keeps (core::track).
    geo::LatLon line_at(int k) const { return lines_[k]; }
    double heading_at(int k) const { return headings_[k]; }

    // Lines placed on an earlier visit, put back in place of the first
    // clean lap placing them. Call after reset(): the bests stay empty,
    // because they are the session's. Takes effect from the next lap start;
    // a lap already being recorded to place them is dropped.
    void place_lines(const geo::LatLon (&at)[kSectors - 1],
                     const double (&heading_deg)[kSectors - 1]);

private:
    struct Instant {
        uint32_t ms = 0;
        float    frac = 0.0f;
    };
    // A point on the lap being recorded, kept relative to its first point so
    // a float holds it to millimetres.
    struct PathPoint {
        float dlat = 0.0f;
        float dlon = 0.0f;
        float dist_ft = 0.0f;
        float course_deg = 0.0f;
    };
    static constexpr size_t kPath = 256;

    void start_lap(Instant at);
    void record(const Fix& f, float course_deg, bool segment_ok);
    void place();
    void close_sector(Instant at, Split& out);

    // Placing.
    PathPoint   path_[kPath];
    size_t      path_n_ = 0;
    size_t      path_stride_ = 1;     // keep one fix in this many
    size_t      path_skip_ = 0;
    geo::LatLon path_origin_{};
    geo::LatLon path_prev_{};
    float       path_dist_ft_ = 0.0f;
    bool        recording_ = false;

    // The lines.
    bool        placed_ = false;
    geo::LatLon lines_[kSectors - 1]{};
    double      headings_[kSectors - 1]{};

    // Timing.
    bool        timing_ = false;
    int         current_ = 0;
    Instant     sector_start_{};
    bool        gapped_ = false;
    geo::LatLon prev_{};
    uint32_t    prev_t_ms_ = 0;
    bool        has_prev_ = false;
    uint32_t    best_[kSectors] = {};
};

// The most a lap (or a session) reached: road speed, rpm, and lean each way.
// Lean counts only above `kMinMph`, so a bike on its side stand, or lying
// down, is not a record.
struct Peaks {
    static constexpr float kMinMph = 10.0f;

    float mph = 0.0f;
    float rpm = 0.0f;
    float lean_left_deg = 0.0f;    // positive degrees
    float lean_right_deg = 0.0f;

    void add(float mph_now, float rpm_now, float lean_deg_now);
    void reset() { *this = Peaks{}; }
    float lean_deg() const { return lean_left_deg > lean_right_deg ? lean_left_deg : lean_right_deg; }
};

}  // namespace core::lap
