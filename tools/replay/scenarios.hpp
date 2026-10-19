// Synthetic rides with a known true speed, for the fusion tests and for
// `replay --scenario`. Header-only and core-only, so the host tests and the
// replay tool run exactly the same rides.
//
// The noise is a guess at a dirt bike, not a measurement of one: vibration
// on the accelerometer, a mounting bias, wheel-pulse jitter, GPS Doppler
// that arrives at 10 Hz and 100 ms late. Replace the guesses with numbers
// from a real 50 Hz log once there is one.
#pragma once
#include "core/fusion/speed.hpp"
#include "core/tach/tach.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace replay {

// Deterministic, so a failing test fails the same way every run.
class Rng {
public:
    explicit Rng(uint32_t seed) : s_(seed ? seed : 1) {}
    float uniform() {
        s_ ^= s_ << 13; s_ ^= s_ >> 17; s_ ^= s_ << 5;
        return static_cast<float>(s_ & 0xFFFFFF) / 16777216.0f;
    }
    float gauss() {
        const float u1 = uniform() + 1e-7f, u2 = uniform();
        return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
    }
private:
    uint32_t s_;
};

struct Sample {
    core::fusion::SpeedInput in;
    float truth_mph = 0.0f;
};

enum class Kind { Ride, Wheelspin, FrontLock, GpsDropout, DeadFront, Parked, NoGps,
                  SpinNoFront, GpsGlitch, Wheelie, Count };

inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Ride:       return "ride";
        case Kind::Wheelspin:  return "wheelspin";
        case Kind::FrontLock:  return "frontlock";
        case Kind::GpsDropout: return "gpsdropout";
        case Kind::DeadFront:  return "deadfront";
        case Kind::Parked:     return "parked";
        case Kind::NoGps:      return "nogps";
        case Kind::SpinNoFront:return "spinnofront";
        case Kind::GpsGlitch:  return "gpsglitch";
        case Kind::Wheelie:    return "wheelie";
        default:               return "?";
    }
}

inline bool kind_from(const char* s, Kind& out) {
    for (int i = 0; i < static_cast<int>(Kind::Count); ++i) {
        if (std::strcmp(s, kind_name(static_cast<Kind>(i))) == 0) {
            out = static_cast<Kind>(i);
            return true;
        }
    }
    return false;
}

// A 40 s ride: away from rest, up to 45 mph, a corner, hard braking, a
// second run, then stopped. Speed in mph at time t seconds.
inline float profile_mph(Kind k, float t) {
    if (k == Kind::Parked) return 0.0f;
    struct Pt { float t, v; };
    static const Pt kPts[] = {{0, 0}, {2, 0}, {8, 45}, {14, 45}, {16, 20}, {20, 20},
                              {22, 40}, {28, 40}, {31, 0}, {34, 0}, {37, 30}, {40, 30}};
    const size_t n = sizeof kPts / sizeof kPts[0];
    if (t <= kPts[0].t) return kPts[0].v;
    for (size_t i = 1; i < n; ++i) {
        if (t <= kPts[i].t) {
            const float f = (t - kPts[i - 1].t) / (kPts[i].t - kPts[i - 1].t);
            return kPts[i - 1].v + f * (kPts[i].v - kPts[i - 1].v);
        }
    }
    return kPts[n - 1].v;
}

constexpr uint32_t kTickMs = 20;           // the logic task's 50 Hz
constexpr uint32_t kDurationMs = 40000;

// Calls fn(const Sample&) for every 20 ms tick of the scenario.
template <typename Fn>
void run(Kind k, uint32_t seed, Fn fn) {
    Rng rng(seed);
    const float bias_g = 0.04f;            // the IMU not quite level
    for (uint32_t t_ms = 0; t_ms <= kDurationMs; t_ms += kTickMs) {
        const float t = static_cast<float>(t_ms) / 1000.0f;
        const float v = profile_mph(k, t);
        const float dv = (profile_mph(k, t + 0.01f) - profile_mph(k, t - 0.01f)) / 0.02f;
        Sample s;
        s.truth_mph = v;
        core::fusion::SpeedInput& in = s.in;
        in.t_ms = t_ms;

        // Vibration grows with speed; the engine is running throughout.
        in.accel_ok = true;
        in.accel_g = dv / core::fusion::kMphPerSecPerG + bias_g +
                     (0.08f + v * 0.003f) * rng.gauss();
        // A wheelie on the second run, 23-26 s: the front up at 30 degrees,
        // so gravity lands on the forward accelerometer as sin 30 = 0.5 g of
        // acceleration that is not happening.
        const bool up = k == Kind::Wheelie && t >= 23.0f && t < 26.0f;
        if (up) in.accel_g += 0.5f;

        // Wheels: pulse timing jitter, and nothing below a walking pace,
        // which is what a few pulses per revolution can resolve.
        const auto wheel = [&](float mph) {
            const float w = mph + 0.25f * rng.gauss();
            return mph < 1.0f ? 0.0f : (w < 0.0f ? 0.0f : w);
        };
        in.front_ok = in.rear_ok = true;
        in.front_mph = wheel(v);
        in.rear_mph = wheel(v);
        if ((k == Kind::Wheelspin || k == Kind::SpinNoFront) && dv > 0.0f && v > 5.0f) {
            // Spin that builds over the run-up, up to 15 mph over.
            in.rear_mph += 15.0f * std::sin(3.14159f * std::fmod(t, 6.0f) / 6.0f);
        }
        if (k == Kind::FrontLock && dv < -10.0f) {
            in.front_mph = 0.0f;                         // locked solid
        }
        if (k == Kind::DeadFront || k == Kind::SpinNoFront) in.front_mph = 0.0f;  // unplugged
        // In the air the front spins down on its bearings and brake drag:
        // a guess, a time constant of 4 s, so ~half its speed by landing.
        // The driven rear keeps turning, a little fast.
        if (up) {
            in.front_mph *= std::exp(-(t - 23.0f) / 4.0f);
            in.rear_mph *= 1.04f;
        }

        // GPS: 10 Hz, 100 ms late, noisier at higher HDOP.
        const bool fix_tick = t_ms % 100 == 0;
        const bool gps_out = (k == Kind::GpsDropout && t > 10.0f && t < 25.0f) ||
                             k == Kind::NoGps;
        in.gps_ok = !gps_out;
        in.hdop = 1.2f;
        in.gps_new = in.gps_ok && fix_tick;
        if (in.gps_new) {
            const float late = profile_mph(k, t - 0.1f);
            const float g = late + 0.4f * in.hdop * rng.gauss();
            in.gps_mph = g < 0.0f ? -g : g;              // Doppler has no sign
            // A multipath fix off a canyon wall, once a second for 3 s.
            if (k == Kind::GpsGlitch && t > 11.0f && t < 14.0f && t_ms % 1000 == 0) {
                in.gps_mph += 25.0f;
            }
        }
        fn(s);
    }
}

// What the dash showed until 1.36: the front wheel whenever its sensor reads
// at all, then the rear, holding the last GPS figure between fixes for when
// it falls through to GPS. Kept as the yardstick: shown_speed_mph with no
// fused figure is exactly this.
struct RawBest {
    float last_gps = 0.0f;
    float operator()(const core::fusion::SpeedInput& in) {
        if (in.gps_new) last_gps = in.gps_mph;
        return core::fusion::shown_speed_mph(in.front_mph, in.front_ok, in.rear_mph,
                                             in.rear_ok, last_gps, in.gps_ok,
                                             0.0f, false, 0);
    }
};

// What the dash shows now: core::fusion::shown_speed_mph with the filter's
// last output, as `ctrl` has it (published by `logic` the tick before).
struct ShownBest {
    float last_gps = 0.0f;
    core::fusion::SpeedOutput prev;
    float operator()(const core::fusion::SpeedInput& in, const core::fusion::SpeedOutput& o) {
        if (in.gps_new) last_gps = in.gps_mph;
        const float v = core::fusion::shown_speed_mph(
            in.front_mph, in.front_ok, in.rear_mph, in.rear_ok, last_gps, in.gps_ok,
            prev.mph, prev.valid, prev.rejected);
        prev = o;
        return v;
    }
};

struct Error {
    double sum_sq = 0.0;
    double max = 0.0;
    size_t n = 0;
    void add(double e) {
        sum_sq += e * e;
        if (std::fabs(e) > max) max = std::fabs(e);
        ++n;
    }
    double rms() const { return n ? std::sqrt(sum_sq / static_cast<double>(n)) : 0.0; }
};

}  // namespace replay
