#include "check.hpp"
#include "core/crash/crash.hpp"
#include <cstring>

using namespace core::crash;

namespace {
// Upright, enabled, at `mph`; one call per 20 ms tick like `logic`.
struct Ride {
    Detector det;
    Input in;
    Ride() { in.enabled = true; in.imu_ok = true; }
    State run(uint32_t for_ms) {
        State s;
        for (uint32_t end = in.t_ms + for_ms; in.t_ms < end;) {
            in.t_ms += 20;
            s = det.update(in);
        }
        return s;
    }
    void hit(float g) { in.hit_g = g; in.hit_ms = in.t_ms; }
    void on_side() { in.g_vert = 0.05f; in.g_lat = 1.0f; in.mph = 0.0f; }
};
}  // namespace

int main() {
    CASE("riding, a hit, then on its side and still: countdown, then sent");
    {
        Ride r;
        r.in.mph = 35.0f;
        r.run(3000);
        r.hit(6.0f);
        r.on_side();
        CHECK(r.run(4900).phase == Phase::Idle);          // down, not long enough
        State s = r.run(200);
        CHECK(s.phase == Phase::Countdown);
        CHECK(s.countdown_s == 30);
        s = r.run(29000);
        CHECK(s.phase == Phase::Countdown && s.countdown_s == 1);
        s = r.run(1100);
        CHECK(s.phase == Phase::Sent);
        CHECK(s.sos_id == 1 && s.ok_id == 0);
        r.det.cancel();                                    // "I'm OK"
        CHECK(r.det.state().phase == Phase::Idle && r.det.state().ok_id == 1);
    }

    CASE("a button during the countdown: never sent, and no re-arm while it lies there");
    {
        Ride r;
        r.in.mph = 35.0f;
        r.run(1000);
        r.hit(5.0f);
        r.on_side();
        CHECK(r.run(5100).phase == Phase::Countdown);
        r.det.cancel();
        const State s = r.run(60000);
        CHECK(s.phase == Phase::Idle && s.sos_id == 0 && s.ok_id == 0);
    }

    CASE("a jump landing: the hit, but the bike stays upright");
    {
        Ride r;
        r.in.mph = 30.0f;
        r.run(1000);
        r.hit(5.5f);
        r.in.mph = 2.0f;                                   // stopped after it
        CHECK(r.run(60000).phase == Phase::Idle);
    }

    CASE("tipped over in the car park: down, no hit, no riding");
    {
        Ride r;
        r.run(1000);
        r.hit(3.5f);                                        // hits the ground
        r.on_side();
        CHECK(r.run(60000).phase == Phase::Idle);
    }

    CASE("a board mounted on its side never arms: driven tilted, a pothole, a red light");
    {
        Ride r;
        r.on_side();
        r.in.mph = 40.0f;
        r.run(10000);
        r.hit(4.5f);
        r.in.mph = 0.0f;
        CHECK(r.run(60000).phase == Phase::Idle);
    }

    CASE("down more than 30 s after the hit does not count");
    {
        Ride r;
        r.in.mph = 40.0f;
        r.run(1000);
        r.hit(8.0f);
        r.in.mph = 1.0f;                                    // upright, stopped
        r.run(31000);
        r.on_side();
        CHECK(r.run(10000).phase == Phase::Idle);
    }

    CASE("riding off ends the countdown; Crash SOS off stops everything");
    {
        Ride r;
        r.in.mph = 25.0f;
        r.run(1000);
        r.hit(4.0f);
        r.on_side();
        CHECK(r.run(5100).phase == Phase::Countdown);
        r.in.g_vert = 1.0f; r.in.g_lat = 0.0f; r.in.mph = 15.0f;
        CHECK(r.run(100).phase == Phase::Idle);

        Ride off;
        off.in.enabled = false;
        off.in.mph = 25.0f;
        off.run(1000);
        off.hit(9.0f);
        off.on_side();
        CHECK(off.run(60000).phase == Phase::Idle);
    }

    CASE("no IMU, no crash");
    {
        Ride r;
        r.in.imu_ok = false;
        r.in.mph = 25.0f;
        r.run(1000);
        r.hit(9.0f);
        r.on_side();
        CHECK(r.run(60000).phase == Phase::Idle);
    }

    CASE("console crash test: the countdown from now, sent when it runs out");
    {
        Ride r;
        r.run(1000);
        r.det.test(r.in.t_ms);
        CHECK(r.run(20).phase == Phase::Countdown);
        CHECK(r.run(30000).phase == Phase::Sent);
    }

    CASE("peak hold keeps the biggest sample for its window");
    {
        PeakHold p;
        p.update(1.0f, 0);
        p.update(7.0f, 10);
        p.update(1.0f, 15);
        CHECK(p.g() == 7.0f && p.ms() == 10);
        p.update(1.0f, 10 + PeakHold::kWindowMs + 1);
        CHECK(p.g() == 1.0f);
    }

    CASE("wire: SOS, OK and the phone's ack");
    {
        char b[64];
        size_t n = sos_line(3, true, 40.0150001, -105.2705, 4, b, sizeof b);
        CHECK(n > 0 && std::strcmp(b, "X 3 40.015000 -105.270500 4") == 0);
        n = sos_line(3, false, 0, 0, 0, b, sizeof b);
        CHECK(n > 0 && std::strcmp(b, "X 3 - - -1") == 0);
        CHECK(ok_line(3, b, sizeof b) > 0 && std::strcmp(b, "X 3 ok") == 0);
        CHECK(sos_line(3, true, 40.0, -105.0, 4, b, 8) == 0);   // too small

        uint16_t id = 0;
        Ack a = Ack::None;
        const auto ack = [&](const char* s) {
            return parse_ack(reinterpret_cast<const uint8_t*>(s), std::strlen(s), id, a);
        };
        CHECK(ack("K 3 1") && id == 3 && a == Ack::Texted);
        CHECK(ack("K 65535 2") && id == 65535 && a == Ack::Failed);
        CHECK(ack("K 12 0\n") && id == 12 && a == Ack::Off);
        CHECK(!ack("K 3 7"));
        CHECK(!ack("K x 1"));
        CHECK(!ack("K 0 1"));
        CHECK(!ack("K 70000 1"));
        CHECK(!ack("T 3 1"));
    }

    return check::report("crash");
}
