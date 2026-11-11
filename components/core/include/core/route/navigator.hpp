// The turn-by-turn state machine (docs/navigation.md §5, §7): a destination
// chosen, a route asked for, following, rerouting, arriving.
//
// Pure. `logic` owns one. It feeds it the rider's commands, the route
// service's answers and the position; it posts whatever request
// take_request() hands it when the mailbox is free; and it follows the
// route the navigator adopted (answered() returning true).
//
// The speed limit: the rider's setting (NAV's picker, or CONFIG -> Nav),
// off, 65 or 55 mph. Every route comes from the graph without roads posted
// over it (or presumed over it: an untagged divided highway is 65, an
// interstate 70). If nothing connects under it, the rider is told and may
// lift it for that destination.
#pragma once
#include <cstdint>
#include "core/route/follow.hpp"
#include "core/route/rgf.hpp"
#include "core/route/route.hpp"

namespace core::route {

enum class Phase : uint8_t {
    NoMap,         // no usable map file
    Idle,          // nothing chosen
    Routing,       // the first route to a destination is being worked out
    NoSlowRoute,   // nothing connects under the speed limit: lift it, or cancel
    Guiding,
    Rerouting,     // off the route: a new one from here is being worked out
    Arrived,
    Failed,        // no route at all; status() says why
};
const char* to_string(Phase p);

struct Request {
    Point    from{};
    Point    to{};
    uint32_t variant_flags = 0;
};

struct NavigatorConfig {
    uint32_t reroute_gap_ms   = 10000;   // at most one reroute this often
    uint32_t arrived_hold_ms  = 30000;   // "Arrived" stays up this long
    FollowConfig follow{};
};

class Navigator {
public:
    explicit Navigator(const NavigatorConfig& c = NavigatorConfig())
        : cfg_(c), follower_(c.follow) {}

    // The route service's word on the map file.
    void set_map(bool ok);
    bool has_map() const { return phase_ != Phase::NoMap; }

    // Go to `to`. False (and nothing changes) without a map or a fix.
    bool start(Point to, Point here, bool fix);
    void cancel();
    void any_speed();        // NoSlowRoute: route again with no speed limit
    void set_avoid_dirt(bool on) { avoid_dirt_ = on; }
    // The fastest road a route may use, mph; 0 for any. Taken by the next
    // request.
    void set_max_mph(uint8_t mph) { max_mph_ = mph; }

    // A request for the mailbox, once. Call only when the mailbox is free.
    bool take_request(Request& out);
    bool waiting() const { return in_flight_ || pending_; }

    // The service's answer to the last request. True: follow `r` from now
    // on (the caller makes it the live route).
    bool answered(Status s, const Route& r, uint32_t now_ms);

    // Each pass, with the live route (null for none) and where the bike is.
    Progress tick(const Route* live, Point here, bool fix, uint32_t now_ms);

    Phase      phase() const { return phase_; }
    Status     status() const { return status_; }
    bool       avoid_dirt() const { return avoid_dirt_; }
    uint8_t    max_mph() const { return max_mph_; }
    // The speed limit as variant flags, 0 for none.
    uint32_t   limit() const { return limit_flags(max_mph_); }
    // Whether the live route should be shown and followed at all.
    bool       following() const;
    Point      destination() const { return dest_; }

private:
    void request_from(Point here);
    uint32_t flags() const;

    NavigatorConfig cfg_;
    Follower follower_;
    Phase    phase_ = Phase::NoMap;
    Status   status_ = Status::Ok;
    bool     avoid_dirt_ = false;
    uint8_t  max_mph_ = 0;
    bool     lifted_ = false;       // the rider lifted the limit for this destination
    Point    dest_{};
    Point    here_{};
    Request  req_{};
    bool     pending_ = false;      // a request not yet posted
    bool     in_flight_ = false;    // posted, not yet answered
    bool     have_route_ = false;   // a route has been adopted for this destination
    bool     want_reroute_ = false; // off the route long enough; asks once the gap allows
    bool     detour_ = false;       // under a limit but on a road over it: this route may use them
    uint32_t last_request_ms_ = 0;
    uint32_t arrived_ms_ = 0;
    uint32_t now_ms_ = 0;
};

}  // namespace core::route
