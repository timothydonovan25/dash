// BLE TPMS: the decoder against an advert as a sensor sends it, and the
// two-wheel tracker with its learn.
#include "check.hpp"
#include "core/tpms/tpms.hpp"

#include <cstring>

using namespace core::tpms;

namespace {
// An 0x0100 sensor's advert: flags, then the manufacturer data. The
// payload is the published example of the family's layout (sensor 1 at
// 80:EA:CA:10:8A:78, 28131 Pa, 27.90 C, 91 %, no alarm), not yet one of
// ours.
const uint8_t kAdv[] = {
    0x02, 0x01, 0x06,
    0x13, 0xFF, 0x00, 0x01, 0x80, 0xEA, 0xCA, 0x10, 0x8A, 0x78,
    0xE3, 0x6D, 0x00, 0x00, 0xE6, 0x0A, 0x00, 0x00, 0x5B, 0x00,
};

Reading reading(uint32_t id, float psi) {
    Reading r;
    r.id = id;
    r.psi = psi;
    return r;
}
}  // namespace

int main() {
    CASE("0x0100 advert decodes");
    {
        Reading r;
        CHECK(decode(kAdv, sizeof kAdv, r));
        CHECK(r.id == 0x108A78);
        CHECK_NEAR(r.psi, 28131.0 / 6894.757, 0.001);
        CHECK_NEAR(r.temp_c, 27.90, 0.001);
        CHECK(r.batt_pct == 91);
        CHECK(!r.alarm);
    }

    CASE("other adverts do not");
    {
        Reading r;
        uint8_t a[sizeof kAdv];
        std::memcpy(a, kAdv, sizeof a);
        a[6] = 0x02;                       // another company
        CHECK(!decode(a, sizeof a, r));
        std::memcpy(a, kAdv, sizeof a);
        a[7] = 0x10;                       // not 0x8n
        CHECK(!decode(a, sizeof a, r));
        std::memcpy(a, kAdv, sizeof a);
        a[21] = 101;                       // battery over 100 %
        CHECK(!decode(a, sizeof a, r));
        CHECK(!decode(kAdv, sizeof kAdv - 1, r));   // cut short
        const uint8_t flags_only[] = {0x02, 0x01, 0x06};
        CHECK(!decode(flags_only, sizeof flags_only, r));
        CHECK(!decode(nullptr, 0, r));
    }

    CASE("unbound sensors are not shown");
    {
        Tracker t;
        const uint32_t ids[2] = {0, 0};
        CHECK(!t.on_reading(reading(5, 20), ids, 100).any);
        CHECK(!t.publish().front.valid && !t.publish().rear.valid);
    }

    CASE("bound sensors fill their wheel, then go stale");
    {
        Tracker t;
        const uint32_t ids[2] = {5, 6};
        t.on_reading(reading(6, 22.5f), ids, 1000);
        CHECK(t.publish().rear.valid);
        CHECK_NEAR(t.publish().rear.psi, 22.5, 0.001);
        CHECK(!t.publish().front.valid);
        CHECK(!t.tick(ids, 1000 + kStaleMs - 1));
        CHECK(t.publish().rear.age_ms == kStaleMs - 1);
        CHECK(t.tick(ids, 1000 + kStaleMs));
        CHECK(!t.publish().rear.valid);
        CHECK(t.publish().rear.psi == 0.0f);   // a gauge visibly drops
    }

    CASE("learn takes the next sensor not on the other wheel");
    {
        Tracker t;
        uint32_t ids[2] = {0, 7};
        t.learn(Wheel::Front, 0);
        CHECK(t.learning());
        CHECK(!t.on_reading(reading(7, 30), ids, 10).any);   // the rear's
        CHECK(t.learning());
        const Tracker::Learned l = t.on_reading(reading(9, 18), ids, 20);
        CHECK(l.any && l.wheel == Wheel::Front && l.id == 9);
        CHECK(!t.learning());
        CHECK(t.publish().front.valid);
        // Before the ConfigSet lands the wheel stays the new sensor's...
        CHECK(!t.tick(ids, 30));
        CHECK(t.publish().front.valid);
        // ...and after it, too.
        ids[0] = 9;
        CHECK(!t.tick(ids, 40));
        t.on_reading(reading(9, 19), ids, 50);
        CHECK_NEAR(t.publish().front.psi, 19, 0.001);
    }

    CASE("a learn times out");
    {
        Tracker t;
        const uint32_t ids[2] = {0, 0};
        t.learn(Wheel::Rear, 100);
        CHECK(!t.tick(ids, 100 + kLearnMs - 1));
        CHECK(t.tick(ids, 100 + kLearnMs));
        CHECK(!t.learning());
        CHECK(!t.on_reading(reading(3, 20), ids, 100 + kLearnMs + 1).any);
    }

    CASE("re-binding a wheel drops the old sensor's reading");
    {
        Tracker t;
        uint32_t ids[2] = {5, 0};
        t.on_reading(reading(5, 20), ids, 0);
        CHECK(t.publish().front.valid);
        ids[0] = 0;                        // cleared in CONFIG
        CHECK(t.tick(ids, 10));
        CHECK(!t.publish().front.valid);
    }

    return check::report("tpms");
}
