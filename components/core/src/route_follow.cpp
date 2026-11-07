#include "core/route/follow.hpp"
#include <cmath>

namespace core::route {

void Follower::reset() {
    last_ = Progress{};
    seg_ = 0;
    off_since_ms_ = 0;
    off_ = asked_ = arrived_ = false;
}

Progress Follower::update(const Route& r, Point pos, bool fix, uint32_t now_ms) {
    if (r.n_pts < 2) {
        reset();
        return last_;
    }
    if (!fix) {
        // Hold the last answer; a gap in the fix is not leaving the route.
        Progress p = last_;
        p.on = false;
        p.reroute = false;
        return p;
    }
    const uint32_t last_seg = r.n_pts - 2;
    const uint32_t lo = seg_ > cfg_.back_segs ? seg_ - cfg_.back_segs : 0;
    const uint32_t hi = seg_ + cfg_.ahead_segs < last_seg ? seg_ + cfg_.ahead_segs : last_seg;
    const double cl = std::cos(pos.lat_e7 * 1e-7 * 3.14159265358979323846 / 180.0);
    double best = 1e18, best_t = 0;
    uint32_t best_seg = seg_;
    for (uint32_t i = lo; i <= hi; ++i) {
        const Point a = r.pts[i].p, b = r.pts[i + 1].p;
        const double ax = (a.lon_e7 - pos.lon_e7) * cl, ay = a.lat_e7 - pos.lat_e7;
        const double dx = (b.lon_e7 - a.lon_e7) * cl, dy = b.lat_e7 - a.lat_e7;
        const double l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? -(ax * dx + ay * dy) / l2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const double ex = ax + dx * t, ey = ay + dy * t;
        const double d2 = ex * ex + ey * ey;
        if (d2 < best) { best = d2; best_seg = i; best_t = t; }
    }
    seg_ = best_seg;
    const RoutePoint& a = r.pts[seg_];
    const RoutePoint& b = r.pts[seg_ + 1];
    // e7 degrees of latitude to decimetres: 1e-7 deg = 0.0111 m.
    const double xtrack_dm = std::sqrt(best) * 0.111194926644559;

    Progress p;
    p.on = true;
    p.seg = seg_;
    p.along_dm = a.dist_dm + uint32_t((b.dist_dm - a.dist_dm) * best_t + 0.5);
    p.xtrack_dm = uint32_t(xtrack_dm + 0.5);
    const uint32_t at_ds = a.time_ds + uint32_t((b.time_ds - a.time_ds) * best_t + 0.5);
    p.remain_dm = r.total_dm > p.along_dm ? r.total_dm - p.along_dm : 0;
    p.remain_ds = r.total_ds > at_ds ? r.total_ds - at_ds : 0;
    p.name_off = a.name_off;
    p.mph = a.mph;
    p.flags = a.flags;
    for (uint32_t c = 0; c < r.n_cues; ++c) {
        if (r.cues[c].dist_dm > p.along_dm || r.cues[c].kind == CueKind::Arrive) {
            p.cue = c;
            p.to_cue_dm = r.cues[c].dist_dm > p.along_dm ? r.cues[c].dist_dm - p.along_dm : 0;
            break;
        }
    }

    p.off_route = p.xtrack_dm > cfg_.off_route_dm;
    if (p.off_route) {
        if (!off_) { off_ = true; off_since_ms_ = now_ms; }
        if (!asked_ && now_ms - off_since_ms_ >= cfg_.off_route_ms) {
            p.reroute = true;
            asked_ = true;
        }
    } else {
        off_ = asked_ = false;
    }
    if (!p.off_route && p.remain_dm <= cfg_.arrive_dm) arrived_ = true;
    p.arrived = arrived_;
    last_ = p;
    return p;
}

}  // namespace core::route
