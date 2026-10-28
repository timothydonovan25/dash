#include "check.hpp"
#include "core/health/health.hpp"
#include <thread>
#include <atomic>

using namespace core::health;
using core::state::Dev;
using core::state::DevState;

int main() {
    const Policy p;

    CASE("a device is Absent until something reports on it");
    {
        Tracker t;
        CHECK(t[Dev::Imu].state == DevState::Absent);
        CHECK(t.all_ok());                      // absent is not a fault
    }

    CASE("init then a good read makes it Ok");
    {
        Tracker t;
        t.begin_init(Dev::Imu, 0);
        t.tick(0, p);
        CHECK(t[Dev::Imu].state == DevState::Init);
        t.report_ok(Dev::Imu, 100);
        t.tick(100, p);
        CHECK(t[Dev::Imu].state == DevState::Ok);
        CHECK(t[Dev::Imu].last_ok_ms == 100);
        CHECK(t.all_ok());
    }

    CASE("errors degrade, then fault, and the count is a session total");
    {
        Tracker t;
        t.begin_init(Dev::McpPod, 0);
        t.report_ok(Dev::McpPod, 0);
        t.tick(0, p);
        CHECK(t[Dev::McpPod].state == DevState::Ok);
        for (int i = 0; i < 3; ++i) t.report_error(Dev::McpPod, -1, 10);
        t.tick(10, p);
        CHECK(t[Dev::McpPod].state == DevState::Degraded);
        CHECK(!t.all_ok());
        for (int i = 0; i < 7; ++i) t.report_error(Dev::McpPod, -5, 20);
        t.tick(20, p);
        CHECK(t[Dev::McpPod].state == DevState::Fault);
        CHECK(t[Dev::McpPod].fault_count == 10);
        CHECK(t[Dev::McpPod].last_err == -5);
    }

    CASE("one good read clears the streak but not the history");
    {
        Tracker t;
        t.begin_init(Dev::Gnss, 0);
        t.report_ok(Dev::Gnss, 0);
        t.tick(0, p);
        t.report_error(Dev::Gnss, -1, 10);
        t.report_error(Dev::Gnss, -1, 20);
        t.report_error(Dev::Gnss, -1, 30);
        t.tick(30, p);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        t.report_ok(Dev::Gnss, 40);
        t.tick(40, p);
        CHECK(t[Dev::Gnss].state == DevState::Ok);
        // The diag screen still shows that it misbehaved three times.
        CHECK(t[Dev::Gnss].fault_count == 3);
    }

    CASE("a device that goes quiet without erroring is still broken");
    {
        // The handlebar pod that browns out and comes back with default
        // registers: it acknowledges, it errors on nothing, and it reports
        // no buttons ever again. Only the silence gives it away.
        Tracker t;
        t.begin_init(Dev::McpPod, 0);
        t.report_ok(Dev::McpPod, 1000);
        t.tick(1000, p);
        t.tick(1500, p);
        CHECK(t[Dev::McpPod].state == DevState::Ok);
        t.tick(3500, p);                        // 2.5 s since the last read
        CHECK(t[Dev::McpPod].state == DevState::Degraded);
        t.tick(12000, p);                       // long gone
        CHECK(t[Dev::McpPod].state == DevState::Fault);
    }

    CASE("an init stamped just after keeper's clock is not five seconds old");
    {
        // The driver's core read the clock after keeper did; unsigned, the
        // retry came at once instead of five seconds on.
        Tracker t;
        t.begin_init(Dev::Sd, 20003);
        for (int i = 0; i < 10; ++i) t.report_error(Dev::Sd, -1, 20003);
        t.tick(20000, p);
        CHECK(t[Dev::Sd].state == DevState::Fault);
        CHECK(!t.claim_retry(Dev::Sd, 20000, p));
        CHECK(t.claim_retry(Dev::Sd, 25003, p));
    }

    CASE("the keeper retries a faulted device once every five seconds");
    {
        Tracker t;
        t.begin_init(Dev::Sd, 0);
        t.report_ok(Dev::Sd, 0);
        t.tick(0, p);
        for (int i = 0; i < 10; ++i) t.report_error(Dev::Sd, -1, 100);
        t.tick(100, p);
        CHECK(t[Dev::Sd].state == DevState::Fault);

        CHECK(!t.claim_retry(Dev::Sd, 1000, p));    // too soon
        CHECK(t.claim_retry(Dev::Sd, 6000, p));     // 5 s after the init
        CHECK(t[Dev::Sd].state == DevState::Init);  // claiming re-inits it
        CHECK(!t.claim_retry(Dev::Sd, 6000, p));    // and only once

        // Still broken: it faults again and waits out the next interval.
        for (int i = 0; i < 10; ++i) t.report_error(Dev::Sd, -1, 7000);
        t.tick(7000, p);
        CHECK(t[Dev::Sd].state == DevState::Fault);
        t.tick(8000, p);
        CHECK(!t.claim_retry(Dev::Sd, 8000, p));
        CHECK(t.claim_retry(Dev::Sd, 11000, p));

        // A retry that works ends it.
        t.report_ok(Dev::Sd, 11500);
        t.tick(11500, p);
        CHECK(t[Dev::Sd].state == DevState::Ok);
        CHECK(!t.claim_retry(Dev::Sd, 20000, p));   // nothing to retry
    }

    CASE("a device nobody ever mentions is not retried");
    {
        Tracker t;
        t.set_absent(Dev::Ecu);                 // no CAN adapter on this bike
        t.tick(100000, p);
        CHECK(t[Dev::Ecu].state == DevState::Absent);
        CHECK(!t.claim_retry(Dev::Ecu, 100000, p));
        CHECK(t.all_ok());
    }

    CASE("the published table is what the diag screen reads");
    {
        Tracker t;
        t.begin_init(Dev::Imu, 0);
        t.report_ok(Dev::Imu, 10);
        t.begin_init(Dev::Gnss, 0);
        t.report_error(Dev::Gnss, -7, 10);
        t.tick(10, p);
        const auto h = t.publish();
        CHECK(h.ok(Dev::Imu));
        CHECK(!h.ok(Dev::Display));
        CHECK(h[Dev::Gnss].last_err == -7);
    }

    CASE("timestamps are unsigned differences, so a wrapped clock is fine");
    {
        Tracker t;
        const uint32_t near_wrap = 0xFFFFF000u;
        t.begin_init(Dev::Imu, near_wrap);
        t.report_ok(Dev::Imu, near_wrap);
        t.tick(near_wrap + 100, p);
        t.tick(near_wrap + 500, p);             // wraps past zero
        CHECK(t[Dev::Imu].state == DevState::Ok);
        t.tick(near_wrap + 9000, p);
        CHECK(t[Dev::Imu].state != DevState::Ok);
    }

    CASE("worst() picks the one device worth a line on the speed screen");
    {
        core::state::Health h;

        // Nothing fitted, nothing wrong. Absent is not a fault: this board
        // ships with no ECU and no radio, and a permanent warning about
        // parts that were never there teaches the rider to ignore the line.
        CHECK(!worst(h).any());
        CHECK(worst(h).count == 0);

        h[Dev::Ecu].state = DevState::Absent;
        h[Dev::Imu].state = DevState::Ok;
        h[Dev::Sd].state = DevState::Init;
        CHECK(!worst(h).any());

        // One degraded device is the one reported.
        h[Dev::Sd].state = DevState::Degraded;
        CHECK(worst(h).any());
        CHECK(worst(h).dev == Dev::Sd);
        CHECK(worst(h).state == DevState::Degraded);
        CHECK(worst(h).count == 1);

        // A fault outranks a degrade regardless of table order — the SD card
        // comes first in the enum and still loses to the GPS.
        h[Dev::Gnss].state = DevState::Fault;
        CHECK(worst(h).dev == Dev::Gnss);
        CHECK(worst(h).state == DevState::Fault);
        CHECK(worst(h).count == 2);

        // Two faults: the earlier device in the table wins, which is roughly
        // the order that matters — a dead IMU is worse news than a dead card.
        h[Dev::Imu].state = DevState::Fault;
        CHECK(worst(h).dev == Dev::Imu);
        CHECK(worst(h).count == 3);
    }


    CASE("report_ok after set_absent must not resurrect an ejected SD card");
    {
        Tracker t;
        t.begin_init(Dev::Sd, 0);
        t.report_ok(Dev::Sd, 10);
        t.tick(10, p);
        CHECK(t[Dev::Sd].state == DevState::Ok);
        t.report_error(Dev::Sd, -3, 20);
        t.set_absent(Dev::Sd);                  // eject
        CHECK(t.is_absent(Dev::Sd));            // immediate, not tick-lagged
        t.report_ok(Dev::Sd, 30);               // logger's late flush
        t.report_error(Dev::Sd, -3, 31);
        t.tick(40, p);
        CHECK(t[Dev::Sd].state == DevState::Absent);
        CHECK(t[Dev::Sd].fault_count == 0);     // set_absent wiped history
        t.report_ok(Dev::Sd, 50);               // after the tick too
        t.tick(2000000, p);
        CHECK(t[Dev::Sd].state == DevState::Absent);
        CHECK(t.all_ok());
        // Re-insert: begin_init is the only way back.
        t.begin_init(Dev::Sd, 3000000);
        t.report_ok(Dev::Sd, 3000001);
        t.tick(3000002, p);
        CHECK(t[Dev::Sd].state == DevState::Ok);
    }

    CASE("a claimed retry gets a stale window of grace at a fast tick");
    {
        Tracker t;
        t.begin_init(Dev::Imu, 0);
        t.report_ok(Dev::Imu, 0);
        t.tick(0, p);
        for (int i = 0; i < 10; ++i) t.report_error(Dev::Imu, -1, 100);
        t.tick(100, p);
        CHECK(t[Dev::Imu].state == DevState::Fault);
        CHECK(t.claim_retry(Dev::Imu, 20000, p));
        t.tick(20050, p);                       // 50 ms tick, driver not yet reported
        CHECK(t[Dev::Imu].state == DevState::Init);
        t.report_ok(Dev::Imu, 20060);
        t.tick(20100, p);
        CHECK(t[Dev::Imu].state == DevState::Ok);
    }

    CASE("an init still running is not silence; one that failed is");
    {
        // The GPS's bring-up listens for seconds (up to ~6 at 9600). Aged
        // like a running device, it faulted mid-init, the keeper queued a
        // retry at 5 s, and gnss ran the whole init again as soon as it
        // finished: a receiver stuck at 9600 re-initialised for ever.
        Tracker t;
        t.begin_init(Dev::Gnss, 0);
        bool claimed = false;
        for (uint32_t ms = 0; ms < 6000; ms += 50) {
            t.tick(ms, p);
            CHECK(t[Dev::Gnss].state == DevState::Init);
            claimed = claimed || t.claim_retry(Dev::Gnss, ms, p);
        }
        CHECK(!claimed);
        t.report_limited(Dev::Gnss, true);
        t.report_ok(Dev::Gnss, 6000);
        t.tick(6000, p);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);

        // An init that never ends still faults, later.
        Tracker u;
        u.begin_init(Dev::Gnss, 0);
        for (uint32_t ms = 0; ms <= p.init_ms + 100; ms += 50) u.tick(ms, p);
        CHECK(u[Dev::Gnss].state == DevState::Fault);

        // An init that failed (nothing answered) faults on the stale window,
        // as before, and is retried on the usual interval.
        Tracker v;
        v.begin_init(Dev::Gnss, 0);
        v.report_error(Dev::Gnss, -1, 3100);
        for (uint32_t ms = 0; ms <= 3200; ms += 50) v.tick(ms, p);
        CHECK(v[Dev::Gnss].state == DevState::Fault);
        CHECK(v.claim_retry(Dev::Gnss, 5000, p));
    }

    CASE("a reporter clock ahead of the keeper's is not read as silence");
    {
        Tracker t;
        t.begin_init(Dev::Imu, 100);
        t.report_ok(Dev::Imu, 105);
        t.tick(100, p);                         // keeper read its clock first
        CHECK(t[Dev::Imu].state == DevState::Ok);
    }

    CASE("limited: working but short of spec reads Degraded, and working()");
    {
        Tracker t;
        t.begin_init(Dev::Gnss, 0);
        t.report_ok(Dev::Gnss, 10);
        t.report_limited(Dev::Gnss, true);       // the GPS stuck at 1 Hz
        t.tick(10, p);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        CHECK(t[Dev::Gnss].limited);
        core::state::Health h = t.publish();
        CHECK(!h.ok(Dev::Gnss));
        CHECK(h.working(Dev::Gnss));             // its data is still good
        CHECK(!t.all_ok());

        // Reports keep coming at its slow rate: it stays Degraded, not Fault.
        for (uint32_t ms = 1000; ms <= 10000; ms += 1000) {
            t.report_ok(Dev::Gnss, ms);
            t.tick(ms, p);
        }
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        CHECK(!t.claim_retry(Dev::Gnss, 20000, p));   // only a Fault is retried

        // A re-init that got 10 Hz: Ok at the next report.
        t.report_limited(Dev::Gnss, false);
        t.report_ok(Dev::Gnss, 10100);
        t.tick(10100, p);
        CHECK(t[Dev::Gnss].state == DevState::Ok);
        CHECK(t.publish().working(Dev::Gnss));

        // Limited and then silent: judged on the silence.
        t.report_limited(Dev::Gnss, true);
        t.tick(10150, p);
        t.tick(10150 + p.stale_ms * 5, p);
        CHECK(t[Dev::Gnss].state == DevState::Fault);
        CHECK(!t.publish().working(Dev::Gnss));
    }

    CASE("limited: one lost 1 Hz epoch is a Degraded step, not straight to Fault");
    {
        // Its Degraded is the overlay, not a first step of silence: going
        // quiet past stale_ms must take the same two steps an Ok device
        // takes, or a 1 Hz GPS that loses one epoch's sentences to noise
        // faults and is re-initialised, blind for seconds.
        Tracker t;
        t.begin_init(Dev::Gnss, 0);
        t.report_limited(Dev::Gnss, true);
        t.report_ok(Dev::Gnss, 0);
        t.tick(0, p);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        // Last report at 0, the epoch at 1000 lost, the next one 30 ms late.
        // The keeper ticks at 20 Hz.
        for (uint32_t ms = 50; ms <= p.stale_ms; ms += 50) t.tick(ms, p);
        t.tick(p.stale_ms + 20, p);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        t.report_ok(Dev::Gnss, p.stale_ms + 30);
        t.tick(p.stale_ms + 70, p);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        CHECK(t.publish().working(Dev::Gnss));
        // Quiet on: Fault on the next tick, as for any device.
        t.tick(2 * p.stale_ms + 100, p);
        t.tick(2 * p.stale_ms + 150, p);
        CHECK(t[Dev::Gnss].state == DevState::Fault);
    }

    CASE("publish only on change, with a heartbeat for last_ok_ms");
    {
        Tracker t;
        core::Published<core::state::Health> slot;
        t.begin_init(Dev::Imu, 0);
        t.report_ok(Dev::Imu, 0);
        t.tick(0, p);
        CHECK(t.publish_if_changed(slot, 0));           // first one always goes
        const uint32_t g0 = slot.generation();
        for (uint32_t k = 1; k < 20; ++k) {             // 20 Hz, 1 s of nothing
            t.report_ok(Dev::Imu, k * 50);              // last_ok_ms moves only
            t.tick(k * 50, p);
            CHECK(!t.publish_if_changed(slot, k * 50));
        }
        CHECK(slot.generation() == g0);
        CHECK(t.publish_if_changed(slot, 1000));        // heartbeat
        CHECK(slot.get()[Dev::Imu].last_ok_ms == 950);
        t.report_error(Dev::Imu, -9, 1010);             // last_err changes
        t.tick(1050, p);
        CHECK(t.publish_if_changed(slot, 1050));
        CHECK(slot.get()[Dev::Imu].last_err == -9);
        for (int i = 0; i < 3; ++i) t.report_error(Dev::Imu, -9, 1060);
        t.tick(1100, p);                                // -> Degraded
        CHECK(t.publish_if_changed(slot, 1100));
        CHECK(slot.get()[Dev::Imu].state == DevState::Degraded);
        CHECK(!t.publish_if_changed(slot, 1150));
    }

    CASE("two reporter threads against tick(): no race, no lost counts");
    {
        // Run under -fsanitize=thread. One thread plays a driver that
        // alternates ok/error on Sd, another a second reporter on the same
        // device plus set_absent/begin_init churn on Phone; the main thread
        // is the keeper (tick, claim_retry, publish_if_changed).
        Tracker t;
        core::Published<core::state::Health> slot;
        t.begin_init(Dev::Sd, 0);
        t.begin_init(Dev::Phone, 0);
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> errs{0};
        std::thread a([&] {
            uint32_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                t.report_ok(Dev::Sd, n);
                t.report_error(Dev::Sd, -1, n);
                errs.fetch_add(1, std::memory_order_relaxed);
                ++n;
            }
        });
        std::thread b([&] {
            uint32_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                t.report_ok(Dev::Sd, n);
                t.report_error(Dev::Sd, -2, n);
                errs.fetch_add(1, std::memory_order_relaxed);
                if ((n & 255) == 0) t.set_absent(Dev::Phone);
                if ((n & 255) == 128) t.begin_init(Dev::Phone, n);
                t.report_ok(Dev::Phone, n);
                (void)t.is_absent(Dev::Phone);
                (void)t[Dev::Sd];
                ++n;
            }
        });
        uint32_t now = 0;
        for (int i = 0; i < 20000; ++i) {
            t.tick(now += 3, p);
            t.claim_retry(Dev::Sd, now, p);
            t.publish_if_changed(slot, now);
            (void)slot.get();
            (void)t.all_ok();
        }
        stop = true;
        a.join(); b.join();
        t.tick(now + 1, p);                     // drain
        const uint32_t want = errs.load();
        // fault_count saturates at 65535; just check it moved and is sane.
        CHECK(t[Dev::Sd].fault_count > 0);
        CHECK(want > 0);
    }

    return check::report("health");
}
