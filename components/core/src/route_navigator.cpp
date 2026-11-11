#include "core/route/navigator.hpp"

namespace core::route {

const char* to_string(Phase p) {
    switch (p) {
        case Phase::NoMap:       return "no map";
        case Phase::Idle:        return "idle";
        case Phase::Routing:     return "routing";
        case Phase::NoSlowRoute: return "no slow route";
        case Phase::Guiding:     return "guiding";
        case Phase::Rerouting:   return "rerouting";
        case Phase::Arrived:     return "arrived";
        case Phase::Failed:      return "failed";
    }
    return "?";
}

void Navigator::set_map(bool ok) {
    if (!ok) {
        phase_ = Phase::NoMap;
        pending_ = have_route_ = false;
        follower_.reset();
    } else if (phase_ == Phase::NoMap) {
        phase_ = Phase::Idle;
    }
}

uint32_t Navigator::flags() const {
    return (lifted_ || detour_ ? 0u : limit()) | (avoid_dirt_ ? kNoDirt : 0u);
}

void Navigator::request_from(Point here) {
    req_ = Request{here, dest_, flags()};
    pending_ = true;
    last_request_ms_ = now_ms_;
}

bool Navigator::start(Point to, Point here, bool fix) {
    if (phase_ == Phase::NoMap || !fix) return false;
    dest_ = to;
    have_route_ = lifted_ = detour_ = false;
    follower_.reset();
    phase_ = Phase::Routing;
    request_from(here);
    return true;
}

void Navigator::cancel() {
    if (phase_ == Phase::NoMap) return;
    phase_ = Phase::Idle;
    pending_ = have_route_ = want_reroute_ = lifted_ = detour_ = false;
    follower_.reset();
}

void Navigator::any_speed() {
    if (phase_ != Phase::NoSlowRoute) return;
    lifted_ = true;
    phase_ = Phase::Routing;
    request_from(here_);
}

bool Navigator::take_request(Request& out) {
    if (!pending_) return false;
    out = req_;
    pending_ = false;
    in_flight_ = true;
    return true;
}

bool Navigator::answered(Status s, const Route&, uint32_t now_ms) {
    in_flight_ = false;
    now_ms_ = now_ms;
    // Cancelled while it was out, or a newer request supersedes it.
    if (pending_ || phase_ == Phase::Idle || phase_ == Phase::NoMap || phase_ == Phase::Arrived)
        return false;
    status_ = s;
    if (s == Status::Ok) {
        have_route_ = true;
        follower_.reset();
        phase_ = Phase::Guiding;
        return true;
    }
    if (s == Status::NoMap) {
        set_map(false);
        return false;
    }
    if ((phase_ == Phase::Rerouting || phase_ == Phase::Routing) && s == Status::NoStart &&
        (req_.variant_flags & kLimitFlags)) {
        // Under a speed limit, but on a road over it (a wrong turn onto
        // the highway, or starting from one): no slower road to start
        // from. Route from here with it lifted; tick() puts it back once
        // the rider is off that road.
        detour_ = true;
        request_from(here_);
        return false;
    }
    if (phase_ == Phase::Rerouting) {
        // Keep following the old route, and try again after the gap.
        phase_ = Phase::Guiding;
        want_reroute_ = true;
        return false;
    }
    if ((req_.variant_flags & kLimitFlags) && (s == Status::NoRoute || s == Status::NoStart || s == Status::NoDest)) {
        phase_ = Phase::NoSlowRoute;
        return false;
    }
    phase_ = Phase::Failed;
    return false;
}

bool Navigator::following() const {
    return have_route_ && (phase_ == Phase::Guiding || phase_ == Phase::Rerouting ||
                           phase_ == Phase::Arrived);
}

Progress Navigator::tick(const Route* live, Point here, bool fix, uint32_t now_ms) {
    now_ms_ = now_ms;
    if (fix) here_ = here;
    if (phase_ == Phase::Arrived && now_ms - arrived_ms_ >= cfg_.arrived_hold_ms) cancel();
    if (!live || !following()) return Progress{};
    const Progress p = follower_.update(*live, here, fix, now_ms);
    if (p.reroute) want_reroute_ = true;
    if (!p.off_route) want_reroute_ = false;
    if (p.arrived && phase_ != Phase::Arrived) {
        phase_ = Phase::Arrived;
        arrived_ms_ = now_ms;
        want_reroute_ = false;
    } else if (want_reroute_ && fix && phase_ == Phase::Guiding && !waiting() &&
               now_ms - last_request_ms_ >= cfg_.reroute_gap_ms) {
        want_reroute_ = detour_ = false;         // off it by an exit of the rider's own
        phase_ = Phase::Rerouting;
        request_from(here);
    } else if (detour_ && p.on && allowed(p.flags, p.cap_mph, limit()) && phase_ == Phase::Guiding && !waiting()) {
        detour_ = false;                         // off the fast road: under the limit from here
        phase_ = Phase::Rerouting;
        request_from(here);
    }
    return p;
}

}  // namespace core::route
