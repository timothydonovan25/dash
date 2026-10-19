// core/fusion stage 1: fused road speed.
//
// The scenario cases ride the same synthetic rides `replay --scenario all`
// prints, over several noise seeds, and hold the filter to the worst error
// seen across them with some headroom. The bounds are about the design,
// not about a real bike: the noise is a guess until there is a 50 Hz log.
#include "check.hpp"
#include <algorithm>
#include "scenarios.hpp"
#include "core/fusion/speed.hpp"

using namespace core::fusion;
using replay::Kind;

namespace {

struct Result {
    replay::Error raw, shown, fused;
    uint8_t suspect_seen = 0;
    uint8_t rejected_seen = 0;
    uint16_t resets = 0;
    bool always_valid_moving = true;
};

Result ride(Kind k, uint32_t seed) {
    Result r;
    SpeedFilter f;
    replay::RawBest best;
    replay::ShownBest shown;
    replay::run(k, seed, [&](const replay::Sample& s) {
        const SpeedOutput o = f.step(s.in);
        r.raw.add(best(s.in) - s.truth_mph);
        r.shown.add(shown(s.in, o) - s.truth_mph);
        r.fused.add((o.valid ? o.mph : 0.0f) - s.truth_mph);
        r.suspect_seen |= o.suspect;
        r.rejected_seen |= o.rejected;
        r.resets = o.resets;
        if (s.truth_mph > 1.0f && !o.valid) r.always_valid_moving = false;
    });
    return r;
}

// The speed the dash shows (core::fusion::shown_speed_mph): worst RMS and
// worst max over seeds 1..8.
void worst_shown(Kind k, double& rms, double& max) {
    rms = max = 0.0;
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        const Result r = ride(k, seed);
        if (r.shown.rms() > rms) rms = r.shown.rms();
        if (r.shown.max > max) max = r.shown.max;
    }
}

// Worst RMS and worst max over seeds 1..8.
void worst(Kind k, double& rms, double& max) {
    rms = max = 0.0;
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        const Result r = ride(k, seed);
        if (r.fused.rms() > rms) rms = r.fused.rms();
        if (r.fused.max > max) max = r.fused.max;
    }
}

SpeedInput at(uint32_t t_ms) {
    SpeedInput in;
    in.t_ms = t_ms;
    return in;
}

}  // namespace

int main() {
    CASE("nothing to go on: not valid, and no number pretending to be one");
    {
        SpeedFilter f;
        SpeedInput in = at(0);
        in.accel_ok = true;                 // the IMU alone cannot start it
        CHECK(!f.step(in).valid);
        in.t_ms = 20;
        CHECK(!f.step(in).valid);
    }

    CASE("starts on the first wheel reading and follows a steady speed");
    {
        SpeedFilter f;
        SpeedOutput o;
        for (uint32_t t = 0; t <= 2000; t += 20) {
            SpeedInput in = at(t);
            in.accel_ok = true;
            in.front_ok = in.rear_ok = true;
            in.front_mph = in.rear_mph = 30.0f;
            o = f.step(in);
        }
        CHECK(o.valid);
        CHECK_NEAR(o.mph, 30.0, 0.05);
        CHECK((o.used & kSrcFront) && (o.used & kSrcRear) && (o.used & kSrcAccel));
        CHECK(o.sigma_mph < 1.0f);
    }

    CASE("coasting on the IMU alone stops being valid after coast_ms");
    {
        SpeedFilter f;
        SpeedConfig cfg;
        SpeedInput in = at(0);
        in.front_ok = true;
        in.front_mph = 20.0f;
        in.accel_ok = true;
        f.step(in, cfg);
        in.front_ok = false;
        SpeedOutput o;
        for (uint32_t t = 20; t <= cfg.coast_ms; t += 20) { in.t_ms = t; o = f.step(in, cfg); }
        CHECK(o.valid);
        CHECK_NEAR(o.mph, 20.0, 0.01);       // no acceleration, no change
        in.t_ms = cfg.coast_ms + 40;
        CHECK(!f.step(in, cfg).valid);
    }

    CASE("the accelerometer's bias is learned, not integrated into speed");
    {
        SpeedFilter f;
        SpeedOutput o;
        for (uint32_t t = 0; t <= 30000; t += 20) {
            SpeedInput in = at(t);
            in.accel_ok = true;
            in.accel_g = 0.05f;              // tilted mount, steady speed
            in.front_ok = in.rear_ok = true;
            in.front_mph = in.rear_mph = 25.0f;
            o = f.step(in);
        }
        CHECK_NEAR(o.bias_g, 0.05, 0.01);
        CHECK_NEAR(o.mph, 25.0, 0.2);
    }

    CASE("a long gap restarts the filter instead of integrating it");
    {
        SpeedFilter f;
        SpeedInput in = at(0);
        in.front_ok = true;
        in.front_mph = 10.0f;
        in.accel_ok = true;
        f.step(in);
        in.t_ms = 5000;
        in.accel_g = 1.0f;                   // would be +110 mph over 5 s
        in.front_mph = 12.0f;
        CHECK_NEAR(f.step(in).mph, 12.0, 0.5);
    }

    CASE("never negative: a CRF250F does not reverse");
    {
        SpeedFilter f;
        SpeedOutput o;
        for (uint32_t t = 0; t <= 400; t += 20) {
            SpeedInput in = at(t);
            in.accel_ok = true;
            in.accel_g = -1.0f;
            in.front_ok = true;
            in.front_mph = 0.0f;
            o = f.step(in);
        }
        CHECK(o.mph >= 0.0f);
    }

    CASE("a 25 mph GPS glitch is rejected, and does not take the wheels out");
    {
        SpeedFilter f;
        SpeedOutput o;
        for (uint32_t t = 0; t <= 3000; t += 20) {
            SpeedInput in = at(t);
            in.accel_ok = true;
            in.front_ok = in.rear_ok = true;
            in.front_mph = in.rear_mph = 30.0f;
            in.gps_ok = true;
            in.gps_new = t % 100 == 0;
            in.gps_mph = t == 2000 ? 55.0f : 30.0f;
            o = f.step(in);
            if (t == 2000) {
                CHECK(o.rejected & kSrcGps);
                CHECK(o.used & kSrcFront);
            }
        }
        CHECK_NEAR(o.mph, 30.0, 0.2);
    }

    CASE("hard braking: the wheels are judged against where GPS is now, not was");
    {
        // 0.8 g from 20 mph, GPS at 10 Hz. Between fixes the last one goes
        // stale by up to 1.4 mph; against the fixed 4 mph allowance below
        // 16 mph, judging the wheels against the bare stale reading threw
        // out good wheels near the end of every fix interval.
        constexpr float kDecel = 0.8f * 21.94f;      // mph per second
        SpeedFilter f;
        SpeedConfig cfg;
        cfg.wheel_gps_mph = 1.0f;                    // tight, to make it show
        int wheel_rejections = 0;
        for (uint32_t t = 0; t <= 3000; t += 20) {
            const float sec = t < 2000 ? 0.0f : (t - 2000) / 1000.0f;
            const float truth = t < 2000 ? 20.0f : std::max(0.0f, 20.0f - kDecel * sec);
            SpeedInput in = at(t);
            in.accel_ok = true;
            in.accel_g = t < 2000 || truth <= 0.0f ? 0.0f : -0.8f;
            in.front_ok = in.rear_ok = true;
            in.front_mph = in.rear_mph = truth;
            in.gps_ok = true;
            in.gps_new = t % 100 == 0;
            in.gps_mph = truth;
            const SpeedOutput o = f.step(in, cfg);
            if (t > 2000 && truth > 3.0f && (o.rejected & (kSrcFront | kSrcRear))) {
                ++wheel_rejections;
            }
        }
        CHECK(wheel_rejections == 0);
    }

    CASE("a source rejected from the very first tick is not suspect at once");
    {
        SpeedFilter f;
        SpeedInput in = at(0);              // t = 0: the clock's first value
        in.accel_ok = true;
        in.front_ok = in.rear_ok = true;
        in.front_mph = 0.0f;
        in.rear_mph = 30.0f;
        in.gps_ok = in.gps_new = true;
        in.gps_mph = 30.0f;
        const SpeedOutput o = f.step(in);
        CHECK(o.rejected & kSrcFront);
        CHECK(!(o.suspect & kSrcFront));
    }

    // --- the synthetic rides -------------------------------------------
    double rms = 0, max = 0;

    CASE("ride: as good as the wheels on a clean ride");
    worst(Kind::Ride, rms, max);
    CHECK(rms < 0.2);
    CHECK(max < 1.5);

    CASE("wheelspin: the spinning rear is not believed");
    worst(Kind::Wheelspin, rms, max);
    CHECK(rms < 0.3);
    CHECK(max < 2.0);
    CHECK(ride(Kind::Wheelspin, 1).rejected_seen & kSrcRear);

    CASE("front lock: braking to a locked front does not read 0 mph");
    worst(Kind::FrontLock, rms, max);
    CHECK(rms < 0.3);
    CHECK(max < 2.0);
    CHECK(ride(Kind::FrontLock, 1).raw.max > 30.0);   // the front, believed (pre-1.36)

    CASE("GPS dropout: the wheels carry it, valid throughout");
    worst(Kind::GpsDropout, rms, max);
    CHECK(rms < 0.2);
    CHECK(ride(Kind::GpsDropout, 1).always_valid_moving);

    CASE("dead front sensor: flagged suspect, the rear and GPS carry on");
    worst(Kind::DeadFront, rms, max);
    CHECK(rms < 0.5);
    CHECK(max < 6.0);                    // the launch, before GPS referees
    CHECK(ride(Kind::DeadFront, 1).suspect_seen & kSrcFront);
    CHECK(ride(Kind::DeadFront, 1).raw.rms() > 20.0);

    CASE("parked: GPS jitter does not make it creep");
    worst(Kind::Parked, rms, max);
    CHECK(max < 0.3);

    CASE("no GPS at all: the wheels and IMU alone");
    worst(Kind::NoGps, rms, max);
    CHECK(rms < 0.2);

    CASE("GPS glitches: rejected, no worse than a clean ride");
    worst(Kind::GpsGlitch, rms, max);
    CHECK(rms < 0.2);
    CHECK(max < 1.5);

    // The known weak spot: no front wheel to compare against, so a spin of
    // less than a quarter of GPS speed gets through. Bounded, not fixed.
    CASE("spin with no front wheel: bounded by the GPS referee");
    worst(Kind::SpinNoFront, rms, max);
    CHECK(rms < 2.5);
    CHECK(max < 12.0);

    // The front up for 3 s on the second run: it spins down to about half
    // its speed, and the accelerometer reads gravity as 0.5 g of drive.
    CASE("wheelie: the fused speed holds through it");
    worst(Kind::Wheelie, rms, max);
    CHECK(rms < 0.8);
    CHECK(max < 3.0);
    CHECK(ride(Kind::Wheelie, 1).raw.max > 15.0);     // the front, believed (pre-1.36)

    // --- the speed the dash shows (shown_speed_mph) ----------------------
    CASE("shown speed: the front wheel first, as before, on a clean ride");
    for (Kind k : {Kind::Ride, Kind::Wheelspin, Kind::GpsDropout, Kind::NoGps,
                   Kind::GpsGlitch, Kind::Parked}) {
        const Result r = ride(k, 3);
        CHECK_NEAR(r.shown.rms(), r.raw.rms(), 1e-9);
        CHECK_NEAR(r.shown.max, r.raw.max, 1e-9);
    }

    CASE("shown speed: a locked front, a wheelie, a dead sensor are not shown");
    worst_shown(Kind::FrontLock, rms, max);
    CHECK(rms < 0.4);
    CHECK(max < 2.5);                     // no tick of 0 mph as it locks
    worst_shown(Kind::Wheelie, rms, max);
    CHECK(rms < 0.8);
    CHECK(max < 3.0);
    worst_shown(Kind::DeadFront, rms, max);
    CHECK(rms < 0.5);
    CHECK(max < 6.0);                     // the launch, before GPS referees

    CASE("shown speed: the order, and when the filter may overrule a wheel");
    {
        // Front, then fused, then rear, then GPS.
        CHECK_NEAR(shown_speed_mph(30, true, 31, true, 29, true, 30.5f, true, 0), 30.0, 1e-6);
        CHECK_NEAR(shown_speed_mph(0, false, 31, true, 29, true, 30.5f, true, 0), 30.5, 1e-6);
        CHECK_NEAR(shown_speed_mph(0, false, 31, true, 29, true, 0, false, 0), 31.0, 1e-6);
        CHECK_NEAR(shown_speed_mph(0, false, 0, false, 29, true, 0, false, 0), 29.0, 1e-6);
        CHECK_NEAR(shown_speed_mph(0, false, 0, false, 0, false, 0, false, 0), 0.0, 1e-6);
        // A front the filter rejected last pass: the fused figure.
        CHECK_NEAR(shown_speed_mph(28, true, 31, true, 30, true, 30.0f, true, kSrcFront),
                   30.0, 1e-6);
        // A front that is plainly wrong now, before the filter has said so.
        CHECK_NEAR(shown_speed_mph(0, true, 45, true, 45, true, 44.5f, true, 0), 44.5, 1e-6);
        // Inside the allowance (4 mph, or a quarter above 16 mph) it stands.
        CHECK_NEAR(shown_speed_mph(10, true, 13, true, 13, true, 13.5f, true, 0), 10.0, 1e-6);
        CHECK_NEAR(shown_speed_mph(36, true, 45, true, 45, true, 44.5f, true, 0), 36.0, 1e-6);
        // With no valid fused speed, nothing overrules a wheel.
        CHECK_NEAR(shown_speed_mph(0, true, 45, true, 45, true, 44.5f, false, kSrcFront),
                   0.0, 1e-6);
        // The rear is only reached with no fused speed, so it is never
        // overruled: the old order, rear before GPS.
        CHECK_NEAR(shown_speed_mph(0, false, 60, true, 45, true, 45.0f, false, kSrcRear),
                   60.0, 1e-6);
    }

    // --- dead wheel sensors -------------------------------------------
    const auto verdicts = [](Kind k, Wheel& front, Wheel& rear, bool& front_ever_dead,
                             bool& rear_ever_dead) {
        SpeedFilter f;
        WheelCheck w;
        front_ever_dead = rear_ever_dead = false;
        replay::run(k, 1, [&](const replay::Sample& s) {
            w.update(s.in, f.step(s.in));
            front_ever_dead |= w.front() == Wheel::Dead;
            rear_ever_dead |= w.rear() == Wheel::Dead;
        });
        front = w.front();
        rear = w.rear();
    };
    Wheel fw, rw;
    bool fdead, rdead;

    CASE("a dead front sensor is found, and the rear is proven good");
    verdicts(Kind::DeadFront, fw, rw, fdead, rdead);
    CHECK(fw == Wheel::Dead);
    CHECK(rw == Wheel::Ok);

    CASE("a dead front is found with only GPS to go on (rear spinning)");
    verdicts(Kind::SpinNoFront, fw, rw, fdead, rdead);
    CHECK(fw == Wheel::Dead);
    CHECK(!rdead);                        // a spin is not a dead sensor

    CASE("wheelspin and a locked front are never called dead");
    verdicts(Kind::Wheelspin, fw, rw, fdead, rdead);
    CHECK(!fdead && !rdead);
    CHECK(fw == Wheel::Ok && rw == Wheel::Ok);
    verdicts(Kind::FrontLock, fw, rw, fdead, rdead);
    CHECK(!fdead && !rdead);

    CASE("parked since key-on: nothing proven either way");
    verdicts(Kind::Parked, fw, rw, fdead, rdead);
    CHECK(fw == Wheel::Unknown && rw == Wheel::Unknown);

    CASE("a dead wheel that comes back is Ok again at once");
    {
        SpeedFilter f;
        WheelCheck w;
        for (uint32_t t = 0; t <= 9000; t += 20) {
            SpeedInput in = at(t);
            in.accel_ok = true;
            in.front_ok = in.rear_ok = true;
            in.rear_mph = 30.0f;
            in.front_mph = t < 7000 ? 0.0f : 30.0f;      // connector reseated at 7 s
            in.gps_ok = true;
            in.gps_new = t % 100 == 0;
            in.gps_mph = 30.0f;
            w.update(in, f.step(in));
            if (t == 4900) CHECK(w.front() != Wheel::Dead);   // not before dead_ms
            if (t == 6980) CHECK(w.front() == Wheel::Dead);
        }
        CHECK(w.front() == Wheel::Ok);
    }

    return check::report("fusion");
}
