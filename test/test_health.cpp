#include "check.hpp"
#include "core/health/health.hpp"

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
        CHECK(t[Dev::Imu].state == DevState::Init);
        t.report_ok(Dev::Imu, 100);
        CHECK(t[Dev::Imu].state == DevState::Ok);
        CHECK(t[Dev::Imu].last_ok_ms == 100);
        CHECK(t.all_ok());
    }

    CASE("errors degrade, then fault, and the count is a session total");
    {
        Tracker t;
        t.begin_init(Dev::McpPod, 0);
        t.report_ok(Dev::McpPod, 0);
        for (int i = 0; i < 3; ++i) t.report_error(Dev::McpPod, -1, 10);
        CHECK(t[Dev::McpPod].state == DevState::Degraded);
        CHECK(!t.all_ok());
        for (int i = 0; i < 7; ++i) t.report_error(Dev::McpPod, -5, 20);
        CHECK(t[Dev::McpPod].state == DevState::Fault);
        CHECK(t[Dev::McpPod].fault_count == 10);
        CHECK(t[Dev::McpPod].last_err == -5);
    }

    CASE("one good read clears the streak but not the history");
    {
        Tracker t;
        t.begin_init(Dev::Gnss, 0);
        t.report_ok(Dev::Gnss, 0);
        t.report_error(Dev::Gnss, -1, 10);
        t.report_error(Dev::Gnss, -1, 20);
        t.report_error(Dev::Gnss, -1, 30);
        CHECK(t[Dev::Gnss].state == DevState::Degraded);
        t.report_ok(Dev::Gnss, 40);
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
        t.tick(1500, p);
        CHECK(t[Dev::McpPod].state == DevState::Ok);
        t.tick(3500, p);                        // 2.5 s since the last read
        CHECK(t[Dev::McpPod].state == DevState::Degraded);
        t.tick(12000, p);                       // long gone
        CHECK(t[Dev::McpPod].state == DevState::Fault);
    }

    CASE("the keeper retries a faulted device once every five seconds");
    {
        Tracker t;
        t.begin_init(Dev::Sd, 0);
        t.report_ok(Dev::Sd, 0);
        for (int i = 0; i < 10; ++i) t.report_error(Dev::Sd, -1, 100);
        CHECK(t[Dev::Sd].state == DevState::Fault);

        CHECK(!t.claim_retry(Dev::Sd, 1000, p));    // too soon
        CHECK(t.claim_retry(Dev::Sd, 6000, p));     // 5 s after the init
        CHECK(t[Dev::Sd].state == DevState::Init);  // claiming re-inits it
        CHECK(!t.claim_retry(Dev::Sd, 6000, p));    // and only once

        // Still broken: it faults again and waits out the next interval.
        for (int i = 0; i < 10; ++i) t.report_error(Dev::Sd, -1, 7000);
        CHECK(t[Dev::Sd].state == DevState::Fault);
        CHECK(!t.claim_retry(Dev::Sd, 8000, p));
        CHECK(t.claim_retry(Dev::Sd, 11000, p));

        // A retry that works ends it.
        t.report_ok(Dev::Sd, 11500);
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
        t.tick(near_wrap + 500, p);             // wraps past zero
        CHECK(t[Dev::Imu].state == DevState::Ok);
        t.tick(near_wrap + 9000, p);
        CHECK(t[Dev::Imu].state != DevState::Ok);
    }

    return check::report("health");
}
