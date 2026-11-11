// The K-line adapter's frames: the format, the live-data decode against two
// published captures, and when the link counts as alive.
#include "check.hpp"
#include "core/ecu/ecu.hpp"

#include <cstring>

using namespace core::ecu;

namespace {
// A CRF250L, key on, engine off (sophienyaa/Honda-Motorcycle-ECU-Tools).
const uint8_t kCrf250lOff[kLiveLen] = {
    0x00, 0x00, 0x1B, 0x00, 0x9E, 0x3D, 0x9E, 0x3D, 0x93, 0x64,
    0xFF, 0xFF, 0x69, 0x00, 0x00, 0x00, 0x80, 0x57, 0x28, 0x80};
// A CBR600RR at speed (andreibaw/Honda_K-Line_KWP2000), padded to length.
const uint8_t kCbrRunning[kLiveLen] = {
    0x2A, 0x30, 0xD4, 0x87, 0x2C, 0x87, 0x9F, 0x3C, 0xE0, 0x5D,
    0xFF, 0xFF, 0x8E, 0x7A, 0x12, 0x0A, 0x80, 0x00, 0x00, 0x00};

struct Buf {
    uint8_t b[64];
    size_t  n;
};
Buf live(uint8_t seq, uint8_t flags = kFlagEcuAwake, const uint8_t* table = kCbrRunning) {
    Buf f;
    f.n = encode(seq, flags, kTableLive, table, kLiveLen, f.b, sizeof f.b);
    return f;
}
const uint8_t kA[kAddrLen] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
const uint8_t kB[kAddrLen] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x61};

// The Link as its readers see it: through the snapshot.
struct Dash {
    Link l;
    bool feed(const Buf& f, uint32_t now, const uint8_t* from = kA) {
        return l.feed(from, f.b, f.n, now);
    }
    bool adapter_online(uint32_t now) const { return core::ecu::adapter_online(l.snapshot(), now); }
    bool reading(Reading& r, uint32_t now) const { return core::ecu::reading(l.snapshot(), r, now); }
    uint32_t lost() const { return l.snapshot().lost; }
    uint32_t rejected() const { return l.snapshot().rejected; }
};
Buf heartbeat(uint8_t seq, uint8_t flags) {
    Buf f;
    f.n = encode(seq, flags, 0, nullptr, 0, f.b, sizeof f.b);
    return f;
}
}  // namespace

int main() {
    CASE("a frame round-trips");
    {
        const Buf f = live(7);
        CHECK(f.n == kHeaderLen + kLiveLen);
        Frame out;
        CHECK(parse(f.b, f.n, out));
        CHECK(out.seq == 7);
        CHECK(out.flags == kFlagEcuAwake);
        CHECK(out.table == kTableLive);
        CHECK(out.len == kLiveLen);
        CHECK(out.payload == f.b + kHeaderLen);
    }

    CASE("what is not one frame is refused");
    {
        Buf f = live(1);
        Frame out;
        CHECK(!parse(nullptr, f.n, out));
        CHECK(!parse(f.b, kHeaderLen - 1, out));
        CHECK(!parse(f.b, f.n - 1, out));       // truncated
        CHECK(!parse(f.b, f.n + 1, out));       // trailing bytes
        f.b[0] ^= 0xFF;
        CHECK(!parse(f.b, f.n, out));           // someone else's
        f = live(1);
        f.b[1] = kVersion + 1;
        CHECK(!parse(f.b, f.n, out));           // a format this build does not know
    }

    CASE("encode refuses what does not fit");
    {
        uint8_t small[kHeaderLen + kLiveLen - 1];
        CHECK(encode(0, 0, kTableLive, kCbrRunning, kLiveLen, small, sizeof small) == 0);
        uint8_t b[32];
        CHECK(encode(0, 0, kTableLive, nullptr, kLiveLen, b, sizeof b) == 0);
    }

    CASE("live data, engine off");
    {
        Reading r;
        CHECK(decode_live(kCrf250lOff, kLiveLen, r));
        CHECK_NEAR(r.rpm, 0.0, 0.01);
        CHECK_NEAR(r.tps_v, 0.527, 0.001);
        CHECK_NEAR(r.tps_pct, 0.0, 0.01);
        CHECK_NEAR(r.engine_c, 21.0, 0.01);
        CHECK_NEAR(r.intake_c, 21.0, 0.01);
        CHECK_NEAR(r.map_kpa, 100.0, 0.01);
        CHECK_NEAR(r.batt_v, 10.5, 0.01);
        CHECK_NEAR(r.speed_kph, 0.0, 0.01);
        CHECK(r.inj_raw == 0);
    }

    CASE("live data, running");
    {
        Reading r;
        CHECK(decode_live(kCbrRunning, kLiveLen, r));
        CHECK_NEAR(r.rpm, 10800.0, 0.01);
        CHECK_NEAR(r.tps_pct, 67.5, 0.01);
        CHECK_NEAR(r.engine_c, 95.0, 0.01);
        CHECK_NEAR(r.intake_c, 20.0, 0.01);
        CHECK_NEAR(r.map_kpa, 93.0, 0.01);
        CHECK_NEAR(r.batt_v, 14.2, 0.01);
        CHECK_NEAR(r.speed_kph, 122.0, 0.01);
        CHECK(r.inj_raw == 0x120A);
    }

    CASE("a short table is not decoded");
    {
        Reading r;
        CHECK(!decode_live(kCbrRunning, kLiveLen - 1, r));
        CHECK(!decode_live(nullptr, kLiveLen, r));
    }

    CASE("throttle never reads over 100 %");
    {
        uint8_t t[kLiveLen] = {};
        t[3] = 0xFF;
        Reading r;
        CHECK(decode_live(t, kLiveLen, r));
        CHECK_NEAR(r.tps_pct, 100.0, 0.01);
    }

    CASE("nothing heard is offline");
    {
        Dash l;
        Reading r;
        CHECK(!l.adapter_online(0));
        CHECK(!l.reading(r, 0));
    }

    CASE("a reading is good until it is stale");
    {
        Dash l;
        const Buf f = live(0);
        CHECK(l.feed(f, 1000));
        Reading r;
        CHECK(l.adapter_online(1000 + kStaleMs));
        CHECK(l.reading(r, 1000 + kStaleMs));
        CHECK_NEAR(r.rpm, 10800.0, 0.01);
        CHECK(!l.adapter_online(1001 + kStaleMs));
        CHECK(!l.reading(r, 1001 + kStaleMs));
    }

    CASE("missed frames are counted, a retry is not taken twice");
    {
        Dash l;
        Buf f = live(10);
        CHECK(l.feed(f, 0));
        CHECK(!l.feed(f, 10));           // the same frame again
        f = live(11);
        CHECK(l.feed(f, 50));
        CHECK(l.lost() == 0);
        f = live(14);                           // 12 and 13 never came
        CHECK(l.feed(f, 100));
        CHECK(l.lost() == 2);
    }

    CASE("the sequence wraps without a loss");
    {
        Dash l;
        Buf f = live(255);
        CHECK(l.feed(f, 0));
        f = live(0);
        CHECK(l.feed(f, 50));
        CHECK(l.lost() == 0);
    }

    CASE("after a silence the sequence starts again");
    {
        Dash l;
        Buf f = live(200);
        CHECK(l.feed(f, 0));
        f = live(200);                          // restarted on the same number
        CHECK(l.feed(f, 5000));
        f = live(3);
        CHECK(l.feed(f, 9000));
        CHECK(l.lost() == 0);
    }

    CASE("a heartbeat keeps the adapter up but is not a reading");
    {
        Dash l;
        Buf f = heartbeat(0, 0);
        CHECK(l.feed(f, 0));
        Reading r;
        CHECK(l.adapter_online(100));
        CHECK(!l.reading(r, 100));
    }

    CASE("the ECU going quiet hides the last reading");
    {
        Dash l;
        Buf f = live(0);
        CHECK(l.feed(f, 0));
        f = heartbeat(1, 0);                    // adapter up, ECU not answering
        CHECK(l.feed(f, 100));
        Reading r;
        CHECK(l.adapter_online(150));
        CHECK(!l.reading(r, 150));
    }

    CASE("another table is taken and ignored");
    {
        Dash l;
        const uint8_t d1[6] = {0x03, 0, 0, 0, 0, 0};
        Buf f;
        f.n = encode(0, kFlagEcuAwake, 0xD1, d1, sizeof d1, f.b, sizeof f.b);
        CHECK(l.feed(f, 0));
        Reading r;
        CHECK(l.adapter_online(10));
        CHECK(!l.reading(r, 10));
    }

    CASE("refused frames are counted");
    {
        Dash l;
        Buf junk{{1, 2, 3, 4}, 4};
        CHECK(!l.feed(junk, 0));
        CHECK(l.rejected() == 1);
        CHECK(!l.adapter_online(0));
        CHECK(!l.l.snapshot().latched);         // junk does not choose the adapter
    }

    CASE("the millisecond clock wrapping does not strand the link");
    {
        Dash l;
        const Buf f = live(0);
        CHECK(l.feed(f, 0xFFFFFF00u));
        Reading r;
        CHECK(l.reading(r, 0x00000010u));       // 272 ms later, across the wrap
    }

    CASE("the first adapter heard is the adapter; another is ignored");
    {
        Dash l;
        CHECK(l.feed(live(0, kFlagEcuAwake, kCrf250lOff), 0, kA));
        CHECK(!l.feed(live(1), 50, kB));        // someone else's bike
        CHECK(l.l.snapshot().foreign == 1);
        Reading r;
        CHECK(l.reading(r, 60));
        CHECK_NEAR(r.rpm, 0.0, 0.01);           // still the first one's data
        CHECK(l.feed(live(1), 100, kA));
        CHECK(l.lost() == 0);                   // the other's frame cost no sequence
    }

    CASE("the C6's message is the address, then the frame");
    {
        Link l;
        const Buf f = live(0);
        uint8_t msg[kAddrLen + sizeof f.b];
        std::memcpy(msg, kA, kAddrLen);
        std::memcpy(msg + kAddrLen, f.b, f.n);
        CHECK(l.feed_forwarded(msg, kAddrLen + f.n, 0));
        CHECK(std::memcmp(l.snapshot().addr, kA, kAddrLen) == 0);
        CHECK(!l.feed_forwarded(msg, kAddrLen - 1, 0));     // not even an address
        CHECK(!l.feed_forwarded(nullptr, 40, 0));
        CHECK(l.snapshot().rejected == 2);
    }

    return check::report("ecu");
}
