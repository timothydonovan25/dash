// Device health and the retry policy (§12). Pure logic — no driver calls,
// no timers, no logging. Drivers report what happened; this decides what it
// means, and the keeper task asks it what to retry.
//
// The rules it exists to enforce:
//
//  - A driver never aborts, never blocks forever, never retries in a tight
//    loop. It reports and carries on.
//  - A device that has stopped answering goes Degraded, then Fault. The
//    keeper retries a Fault device once every 5 s, which turns a loose
//    GH1.25 connector into a self-healing problem instead of a dead dash.
//  - A widget bound to a device that is not Ok shows `--`. A stale value or
//    a zero on a dashboard is worse than no value: 0 psi and "no idea" look
//    identical, and only one of them means stop riding.
#pragma once
#include <atomic>
#include <cstdint>
#include "core/state/published.hpp"
#include "core/state/vehicle.hpp"

namespace core::health {

struct Policy {
    // Consecutive errors before a device stops being trusted.
    uint16_t degrade_after = 3;
    uint16_t fault_after   = 10;

    // A device that has not reported success in this long is stale even if
    // nothing reported an error — the I2C pod that browns out and comes back
    // with default registers looks alive and answers nothing.
    uint32_t stale_ms = 2000;

    // How often the keeper re-initialises a faulted device.
    uint32_t retry_every_ms = 5000;
};

// The one device most worth telling the rider about, for a screen that has
// room for a line and not for a table.
//
// Only Fault and Degraded count. Ok and Absent are not problems, and Init is
// transient by construction — `tick()` moves anything that stays quiet on to
// Degraded, so a device cannot sit in Init and go unreported.
struct Worst {
    state::Dev      dev   = state::Dev::Count;
    state::DevState state = state::DevState::Ok;
    // How many devices are unhealthy in total, so a screen can say "and two
    // others" without walking the table again.
    uint8_t         count = 0;

    bool any() const { return count > 0; }
};

// Ties break by Dev order, which is roughly the order that matters: a dead
// IMU is worse news than a dead SD card.
Worst worst(const state::Health& h);

// Threading. Many tasks on both cores report into one Tracker, so it is
// built so that none of them shares a read-modify-write with another:
//
//   any task     begin_init, set_absent, report_ok, report_error, is_absent,
//                operator[], publish, all_ok. Each touches only that
//                device's own atomics.
//   keeper only  tick, claim_retry, publish_if_changed. The one deriver of
//                DevState and fault_count and the one writer of the health
//                slot, which is rule 2.
//
// Reporters leave facts (when it last worked, how many errors in a row, the
// last error code) and the keeper turns them into Ok / Degraded / Fault on
// its next tick. So a state read here lags a report by up to one tick.
class Tracker {
public:
    // Boot: everything is Absent until something says otherwise.
    void begin_init(state::Dev d, uint32_t t_ms);
    // Takes effect for reporters at once, not at the next tick: an ejected
    // card stays ejected however late the logger's last flush reports on it.
    // Only begin_init() brings a device back.
    void set_absent(state::Dev d);
    bool is_absent(state::Dev d) const;

    // Called by a driver on every successful transaction. Ignored while the
    // device is absent.
    void report_ok(state::Dev d, uint32_t t_ms);

    // Called by a driver on a failed one. `err` is the driver's own code,
    // kept verbatim for the diag screen — this layer does not interpret it.
    void report_error(state::Dev d, int32_t err, uint32_t t_ms);

    // Keeper, once per tick before reading the table. Turns reports into
    // states, and is what notices a device that went quiet without ever
    // reporting an error.
    void tick(uint32_t t_ms, const Policy& p);

    // Keeper: is it time to re-init this device? Records the attempt, so
    // asking is the same as claiming it.
    bool claim_retry(state::Dev d, uint32_t t_ms, const Policy& p);

    // Keeper: publish the table if a state, fault count or error code moved,
    // or at least every `heartbeat_ms` so last_ok_ms on DIAG stays current.
    // True if it published.
    bool publish_if_changed(Published<state::Health>& slot, uint32_t t_ms,
                            uint32_t heartbeat_ms = 1000);

    // By value, assembled from the atomics, so there is no record to tear.
    state::DevHealth operator[](state::Dev d) const;
    state::Health publish() const;

    // True when everything that is fitted is Ok. The header status dot.
    bool all_ok() const;

private:
    struct Cell {
        // Facts, written by reporters.
        std::atomic<uint32_t> last_ok_ms{0};
        std::atomic<uint32_t> ok_gen{0};
        std::atomic<uint32_t> streak{0};
        std::atomic<uint32_t> err_total{0};
        std::atomic<int32_t>  last_err{0};
        // Intent, written by begin_init and set_absent.
        std::atomic<bool>     absent{true};
        std::atomic<bool>     init_pending{false};
        std::atomic<uint32_t> init_t_ms{0};
        std::atomic<uint32_t> ok_floor{0};
        // Derived, written by the keeper and read by anyone.
        std::atomic<uint8_t>  state{static_cast<uint8_t>(state::DevState::Absent)};
        std::atomic<uint16_t> fault_count{0};
        // The keeper's own bookkeeping.
        uint32_t ok_seen = 0;
        uint32_t err_seen = 0;
        uint32_t last_retry_ms = 0;
    };

    Cell          cell_[state::kDevCount];
    state::Health last_pub_{};
    uint32_t      last_pub_ms_ = 0;
    bool          published_once_ = false;
};

}  // namespace core::health
