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

namespace {
constexpr auto kRlx = std::memory_order_relaxed;
constexpr auto kRel = std::memory_order_release;
constexpr auto kAcq = std::memory_order_acquire;
}  // namespace

void Tracker::begin_init(Dev d, uint32_t t_ms) {
    Cell& c = cell_[ix(d)];
    c.absent.store(false, kRlx);
    c.init_t_ms.store(t_ms, kRlx);
    // Oks from before this init do not count towards it.
    c.ok_floor.store(c.ok_gen.load(kRlx), kRlx);
    c.streak.store(0, kRlx);
    c.last_ok_ms.store(t_ms, kRlx);          // give it the grace of one stale window
    c.init_pending.store(true, kRel);
}

void Tracker::set_absent(Dev d) {
    Cell& c = cell_[ix(d)];
    c.init_pending.store(false, kRlx);
    c.absent.store(true, kRel);
}

bool Tracker::is_absent(Dev d) const { return cell_[ix(d)].absent.load(kAcq); }

void Tracker::report_ok(Dev d, uint32_t t_ms) {
    Cell& c = cell_[ix(d)];
    if (c.absent.load(kAcq)) return;
    c.last_ok_ms.store(t_ms, kRlx);
    c.streak.store(0, kRlx);
    c.ok_gen.fetch_add(1, kRel);             // publishes the two stores above
}

void Tracker::report_limited(Dev d, bool limited) {
    cell_[ix(d)].limited.store(limited, kRel);
}

void Tracker::report_error(Dev d, int32_t err, uint32_t t_ms) {
    (void)t_ms;
    Cell& c = cell_[ix(d)];
    c.last_err.store(err, kRlx);
    c.streak.fetch_add(1, kRlx);
    // fault_count is a session total, not a streak. It is what the diag
    // screen shows, and a device that has recovered ten times is still a
    // device worth looking at when the bike is on the stand.
    c.err_total.fetch_add(1, kRel);
}

void Tracker::tick(uint32_t t_ms, const Policy& p) {
    for (uint8_t i = 0; i < state::kDevCount; ++i) {
        Cell& c = cell_[i];
        const DevState was = static_cast<DevState>(c.state.load(kRlx));

        if (c.absent.load(kAcq)) {
            // Not fitted, not a fault, and nothing it said before counts.
            if (was != DevState::Absent) {
                c.state.store(static_cast<uint8_t>(DevState::Absent), kRlx);
                c.fault_count.store(0, kRlx);
            }
            c.err_seen = c.err_total.load(kAcq);
            c.ok_seen = c.ok_gen.load(kAcq);
            continue;
        }

        const uint32_t total = c.err_total.load(kAcq);
        const uint32_t fresh = total - c.err_seen;
        c.err_seen = total;
        if (fresh) {
            const uint32_t fc = c.fault_count.load(kRlx) + fresh;
            c.fault_count.store(fc > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(fc), kRlx);
        }

        // A Degraded that is only the limited overlay is an Ok to the rules
        // below: silence must take its two steps from there, or a 1 Hz GPS
        // that loses one epoch goes straight to Fault and a re-init.
        DevState st = (was == DevState::Degraded && c.limit_shown) ? DevState::Ok : was;
        // Both, always: short-circuited, the first tick from Absent left
        // init_pending set and the next one replayed the init.
        const bool init = c.init_pending.exchange(false, kAcq);
        if (st == DevState::Absent || init) {
            st = DevState::Init;
            c.last_retry_ms = c.init_t_ms.load(kRlx);
            c.ok_seen = c.ok_floor.load(kRlx);
        }

        const uint32_t gen = c.ok_gen.load(kAcq);
        if (gen != c.ok_seen) {
            c.ok_seen = gen;
            st = DevState::Ok;
        }

        // A Fault is not aged: only an ok or the keeper's retry moves it.
        if (st != DevState::Fault) {
            // Signed: a reporter may have stamped a clock read taken after
            // ours, which unsigned would call 49 days of silence.
            int32_t quiet = static_cast<int32_t>(t_ms - c.last_ok_ms.load(kRlx));
            if (quiet < 0) quiet = 0;
            const uint32_t q = static_cast<uint32_t>(quiet);
            const uint32_t n = c.streak.load(kRlx);
            const uint32_t window = st == DevState::Init && n == 0 ? p.init_ms : p.stale_ms;
            if (q > window) {
                st = (st == DevState::Degraded || q > window * 4) ? DevState::Fault
                                                                  : DevState::Degraded;
            } else if (n >= p.fault_after) {
                st = DevState::Fault;
            } else if (n >= p.degrade_after) {
                st = DevState::Degraded;
            }
        }
        // Last, and only over an Ok: a limited device that also errs or goes
        // quiet is judged on that.
        c.limit_shown = st == DevState::Ok && c.limited.load(kAcq);
        if (c.limit_shown) st = DevState::Degraded;
        c.state.store(static_cast<uint8_t>(st), kRlx);
    }
}

bool Tracker::claim_retry(Dev d, uint32_t t_ms, const Policy& p) {
    Cell& c = cell_[ix(d)];
    if (static_cast<DevState>(c.state.load(kRlx)) != DevState::Fault) return false;
    if (t_ms - c.last_retry_ms < p.retry_every_ms) return false;
    c.last_retry_ms = t_ms;
    c.streak.store(0, kRlx);
    // The grace begin_init gives: without it the next tick finds the
    // long-dead last_ok and faults the device again before its driver has
    // had a chance to report.
    c.last_ok_ms.store(t_ms, kRlx);
    c.state.store(static_cast<uint8_t>(DevState::Init), kRlx);
    return true;
}

bool Tracker::publish_if_changed(Published<state::Health>& slot, uint32_t t_ms,
                                 uint32_t heartbeat_ms) {
    const state::Health h = publish();
    bool changed = !published_once_ || t_ms - last_pub_ms_ >= heartbeat_ms;
    for (uint8_t i = 0; i < state::kDevCount && !changed; ++i) {
        changed = h.dev[i].state != last_pub_.dev[i].state ||
                  h.dev[i].limited != last_pub_.dev[i].limited ||
                  h.dev[i].fault_count != last_pub_.dev[i].fault_count ||
                  h.dev[i].last_err != last_pub_.dev[i].last_err;
    }
    if (!changed) return false;
    slot.publish(h);
    last_pub_ = h;
    last_pub_ms_ = t_ms;
    published_once_ = true;
    return true;
}

DevHealth Tracker::operator[](Dev d) const {
    const Cell& c = cell_[ix(d)];
    DevHealth h;
    h.state = static_cast<DevState>(c.state.load(kRlx));
    h.limited = c.limited.load(kRlx);
    h.fault_count = c.fault_count.load(kRlx);
    h.last_ok_ms = c.last_ok_ms.load(kRlx);
    h.last_err = c.last_err.load(kRlx);
    return h;
}

state::Health Tracker::publish() const {
    state::Health h;
    for (uint8_t i = 0; i < state::kDevCount; ++i) h.dev[i] = (*this)[static_cast<Dev>(i)];
    return h;
}

bool Tracker::all_ok() const {
    for (uint8_t i = 0; i < state::kDevCount; ++i) {
        const DevState s = static_cast<DevState>(cell_[i].state.load(kRlx));
        if (s != DevState::Ok && s != DevState::Absent) return false;
    }
    return true;
}

}  // namespace core::health
