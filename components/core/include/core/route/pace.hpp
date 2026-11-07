// Rally pace notes from a route's shape (docs/navigation.md §7.2): the
// bends along a road, between the turn cues, graded 1 (hairpin tight) to 6
// (a kink), with "into" when one runs straight into the next, and
// "tightens", "opens", "long", "hairpin".
//
// The route's polyline is resampled every 5 m along its own distance
// (RoutePoint::dist_dm, which the follower's along_dm counts in), the
// heading change across 20 m either side of a sample is its curvature, and
// a bend is a run of one sign above a threshold. The grade is the tightest
// radius in it. Worked out for a window of the route at a time, streaming,
// so a 300 km route needs no buffer. No allocation, no ESP header.
//
// shortcut: grades come from OSM geometry alone; a road drawn with few
// points reads sharper at its corners than it is, and there is no
// elevation, so no crests. Tune the thresholds from rides.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/route/route.hpp"

namespace core::route {

enum PaceFlag : uint8_t {
    kPaceInto     = 1,    // straight on from the note before, no gap
    kPaceTightens = 2,
    kPaceOpens    = 4,
    kPaceLong     = 8,
    kPaceHairpin  = 16,
};

struct PaceNote {
    uint32_t at_dm = 0;     // where the bend starts, along the route
    uint32_t len_dm = 0;
    uint16_t deg = 0;       // how far it turns
    int8_t   dir = 0;       // +1 right, -1 left
    uint8_t  grade = 0;     // 1 tightest .. 6 a kink
    uint8_t  flags = 0;     // PaceFlag
};

struct PaceConfig {
    uint32_t step_dm = 50;        // resampling
    uint8_t  half = 4;            // samples either side for the heading change (20 m)
    float    enter_per_m = 1.0f / 250;   // a bend starts above this curvature
    float    exit_per_m = 1.0f / 400;    // and ends below it
    uint16_t min_deg = 15;        // less than this is not a note
    uint32_t into_dm = 300;       // a gap up to this is "into"
    uint32_t cue_dm = 300;        // a bend this near a turn cue is the turn itself
};

// Grade for a radius: the 1..6 scale.
uint8_t pace_grade(float radius_m);

// The notes of bends ending at or after `from_dm` and starting before
// `to_dm`, in order. Returns how many were written (at most `cap`).
size_t pace_notes(const Route& r, uint32_t from_dm, uint32_t to_dm, PaceNote* out, size_t cap,
                  const PaceConfig& cfg = PaceConfig());

// "R4", "into L3 tightens", "L1 hairpin", "R5 long". Returns the length.
size_t format_note(const PaceNote& n, char* out, size_t cap);

}  // namespace core::route
