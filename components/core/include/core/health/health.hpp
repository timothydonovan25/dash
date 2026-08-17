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
#include <cstdint>
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

class Tracker {
public:
    // Boot: everything is Absent until something says otherwise.
    void begin_init(state::Dev d, uint32_t t_ms);
    void set_absent(state::Dev d);

    // Called by a driver on every successful transaction.
    void report_ok(state::Dev d, uint32_t t_ms);

    // Called by a driver on a failed one. `err` is the driver's own code,
    // kept verbatim for the diag screen — this layer does not interpret it.
    void report_error(state::Dev d, int32_t err, uint32_t t_ms);

    // Call once per keeper tick before reading the table. This is what
    // notices a device that went quiet without ever reporting an error.
    void tick(uint32_t t_ms, const Policy& p);

    // Keeper: is it time to re-init this device? Records the attempt, so
    // asking is the same as claiming it.
    bool claim_retry(state::Dev d, uint32_t t_ms, const Policy& p);

    const state::DevHealth& operator[](state::Dev d) const;
    state::Health publish() const { return h_; }

    // True when everything that is fitted is Ok. The header status dot.
    bool all_ok() const;

private:
    void apply(state::Dev d, const Policy& p);

    state::Health h_{};
    Policy        policy_{};
    uint16_t      errors_[state::kDevCount] = {};
    uint32_t      last_retry_ms_[state::kDevCount] = {};
};

}  // namespace core::health
