// Following a route (docs/navigation.md §5): where the bike is along it,
// the next cue, what is left, and whether the rider has left it.
//
// Runs in `logic` on every pass. Pure: the position comes in (fused
// between GPS fixes by the caller, or cues feel a second late at 1 Hz),
// and so does the time.
#pragma once
#include <cstdint>
#include "core/route/route.hpp"

namespace core::route {

struct FollowConfig {
    uint32_t off_route_dm = 500;     // 50 m from the line ...
    uint32_t off_route_ms = 5000;    // ... for 5 s: a reroute. Tune on the road.
    uint32_t arrive_dm    = 300;     // within 30 m of the end
    uint32_t back_segs    = 8;       // how far back along the line to look
    uint32_t ahead_segs   = 400;     // and forward
};

struct Progress {
    bool     on        = false;   // a route is being followed and there is a fix
    bool     off_route = false;   // beyond off_route_dm right now
    bool     reroute   = false;   // true on the one update the off-route timer runs out
    bool     arrived   = false;   // latched until reset()
    uint32_t seg       = 0;       // the segment the bike is on
    uint32_t along_dm  = 0;       // from the route's start
    uint32_t xtrack_dm = 0;       // distance from the line
    uint32_t cue       = kNone;   // the next cue ahead, an index into Route::cues
    uint32_t to_cue_dm = 0;
    uint32_t remain_dm = 0;
    uint32_t remain_ds = 0;       // at the graph's speeds
    uint32_t name_off  = 0;       // the road the bike is on
    uint8_t  mph       = 0;       // its tagged limit, 0 unknown
    uint8_t  flags     = 0;       // its Attr flags
};

class Follower {
public:
    explicit Follower(const FollowConfig& c = FollowConfig()) : cfg_(c) {}

    void reset();     // a new route
    Progress update(const Route& r, Point pos, bool fix, uint32_t now_ms);

private:
    FollowConfig cfg_;
    Progress last_{};
    uint32_t seg_ = 0;
    uint32_t off_since_ms_ = 0;
    bool off_ = false;
    bool asked_ = false;
    bool arrived_ = false;
};

}  // namespace core::route
