#include "core/health/health.hpp"

namespace core::health {

using state::Dev;
using state::DevHealth;
using state::DevState;

namespace {
inline uint8_t ix(Dev d) { return static_cast<uint8_t>(d); }
}  // namespace

namespace {
// Fault outranks Degraded; everything else is not a problem.
inline uint8_t rank(DevState s) {
    if (s == DevState::Fault) return 2;
    if (s == DevState::Degraded) return 1;
    return 0;
}
}  // namespace

Worst worst(const state::Health& h) {
    Worst w;
    uint8_t best = 0;
    for (uint8_t i = 0; i < state::kDevCount; ++i) {
        const Dev d = static_cast<Dev>(i);
        const uint8_t r = rank(h[d].state);
        if (r == 0) continue;
        ++w.count;
        if (r > best) {          // strictly greater, so ties keep the first
            best = r;
            w.dev = d;
            w.state = h[d].state;
        }
    }
    return w;
}

void Tracker::begin_init(Dev d, uint32_t t_ms) {
    DevHealth& e = h_[d];
    e.state = DevState::Init;
    e.last_ok_ms = t_ms;          // give it the grace of one stale window
    errors_[ix(d)] = 0;
    last_retry_ms_[ix(d)] = t_ms;
}

void Tracker::set_absent(Dev d) {
    DevHealth& e = h_[d];
    e = DevHealth{};
    errors_[ix(d)] = 0;
}

void Tracker::report_ok(Dev d, uint32_t t_ms) {
    DevHealth& e = h_[d];
    e.last_ok_ms = t_ms;
    e.state = DevState::Ok;
    errors_[ix(d)] = 0;
    // fault_count is a session total, not a streak. It is what the diag
    // screen shows, and a device that has recovered ten times is still a
    // device worth looking at when the bike is on the stand.
}

void Tracker::report_error(Dev d, int32_t err, uint32_t t_ms) {
    DevHealth& e = h_[d];
    e.last_err = err;
    if (e.fault_count != UINT16_MAX) ++e.fault_count;
    if (errors_[ix(d)] != UINT16_MAX) ++errors_[ix(d)];
    (void)t_ms;
    apply(d, policy_);
}

void Tracker::apply(Dev d, const Policy& p) {
    DevHealth& e = h_[d];
    if (e.state == DevState::Absent) return;      // not fitted, not a fault
    const uint16_t n = errors_[ix(d)];
    if (n >= p.fault_after)        e.state = DevState::Fault;
    else if (n >= p.degrade_after) e.state = DevState::Degraded;
}

void Tracker::tick(uint32_t t_ms, const Policy& p) {
    policy_ = p;
    for (uint8_t i = 0; i < state::kDevCount; ++i) {
        const Dev d = static_cast<Dev>(i);
        DevHealth& e = h_[d];
        if (e.state == DevState::Absent || e.state == DevState::Fault) continue;

        // Unsigned, so a wrapped millisecond counter reads as a small gap
        // rather than 49 days of silence.
        const uint32_t quiet = t_ms - e.last_ok_ms;
        if (quiet > p.stale_ms) {
            e.state = (e.state == DevState::Degraded || quiet > p.stale_ms * 4)
                          ? DevState::Fault
                          : DevState::Degraded;
        } else {
            apply(d, p);
        }
    }
}

bool Tracker::claim_retry(Dev d, uint32_t t_ms, const Policy& p) {
    DevHealth& e = h_[d];
    if (e.state != DevState::Fault) return false;
    if (t_ms - last_retry_ms_[ix(d)] < p.retry_every_ms) return false;
    last_retry_ms_[ix(d)] = t_ms;
    errors_[ix(d)] = 0;
    e.state = DevState::Init;
    return true;
}

const DevHealth& Tracker::operator[](Dev d) const { return h_[d]; }

bool Tracker::all_ok() const {
    for (uint8_t i = 0; i < state::kDevCount; ++i) {
        const DevState s = h_[static_cast<Dev>(i)].state;
        if (s != DevState::Ok && s != DevState::Absent) return false;
    }
    return true;
}

}  // namespace core::health
