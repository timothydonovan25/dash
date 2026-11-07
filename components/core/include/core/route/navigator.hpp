// The turn-by-turn state machine (docs/navigation.md §5, §7): a destination
// chosen, a route asked for, the 65 mph question, following, rerouting,
// arriving.
//
// Pure. `logic` owns one. It feeds it the rider's commands, the route
// service's answers and the position; it posts whatever request
// take_request() hands it when the mailbox is free; and it follows the
// route the navigator adopted (answered() returning true).
//
// The 65 mph rule: the bike can't hold 65. The first route to a
// destination comes from the full graph; if any of it is on a 65 mph road
// (tagged, or a divided highway with no tag), the rider is asked. Avoid
// asks again on the graph without those roads; keep follows the first. The
// answer stands for that destination's reroutes. If nothing connects
// without them, the rider is told and may still keep the fast route.
#pragma once
#include <cstdint>
#include "core/route/follow.hpp"
#include "core/route/route.hpp"

namespace core::route {

enum class Phase : uint8_t {
    NoMap,         // no usable map file
    Idle,          // nothing chosen
    Routing,       // the first route to a destination is being worked out
    AskFast,       // it uses 65 mph roads: avoid or keep? (followed meanwhile)
    NoSlowRoute,   // avoiding them leaves no route: keep, or cancel
    Guiding,
    Rerouting,     // off the route: a new one from here is being worked out
    Arrived,
    Failed,        // no route at all; status() says why
};
const char* to_string(Phase p);

enum class FastChoice : uint8_t { Unasked, Keep, Avoid };

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
    void avoid_fast();       // AskFast: route again without 65 mph roads
    void keep_fast();        // AskFast or NoSlowRoute: use the fast route
    void set_avoid_dirt(bool on) { avoid_dirt_ = on; }

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
    FastChoice fast_choice() const { return choice_; }
    bool       avoid_dirt() const { return avoid_dirt_; }
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
    FastChoice choice_ = FastChoice::Unasked;
    bool     avoid_dirt_ = false;
    Point    dest_{};
    Point    here_{};
    Request  req_{};
    bool     pending_ = false;      // a request not yet posted
    bool     in_flight_ = false;    // posted, not yet answered
    bool     have_route_ = false;   // a route has been adopted for this destination
    bool     want_reroute_ = false; // off the route long enough; asks once the gap allows
    bool     fast_detour_ = false;  // avoiding 65 mph roads but on one: this route may use them
    uint32_t last_request_ms_ = 0;
    uint32_t arrived_ms_ = 0;
    uint32_t now_ms_ = 0;
};

}  // namespace core::route
