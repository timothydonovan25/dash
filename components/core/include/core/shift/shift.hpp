// RPM to shift-lamp pattern. Pure logic.
//
// Eight lamps light progressively between base_rpm and base_rpm + span.
// Hysteresis stops a lamp flickering when RPM sits exactly on a threshold,
// which is what it does at a steady throttle.
#pragma once
#include <cstdint>

namespace core::shift {

inline constexpr int kLampCount = 8;

// What the PCA9955B says about its outputs (datasheet §7.3.15–16), sorted
// into the lamps and the backlight. Bit n of a lamp mask is lamp n+1, as in
// pattern(); bit n of a backlight mask is string n+1. Health, not policy:
// nothing here turns anything off.
//
// The chip only tests an output that is on, at a PWM of 8..254, so a lamp
// that has not been lit since the last clear reads clean whatever its state.
struct LedFaults {
    uint8_t lamp_open = 0;
    uint8_t lamp_short = 0;
    uint8_t backlight_open = 0;
    uint8_t backlight_short = 0;   // see datasheets/PCA9955B-USAGE.md: unproven
    bool    overtemp = false;      // the chip has blanked every output itself

    bool operator==(const LedFaults& o) const {
        return lamp_open == o.lamp_open && lamp_short == o.lamp_short &&
               backlight_open == o.backlight_open &&
               backlight_short == o.backlight_short && overtemp == o.overtemp;
    }
    bool operator!=(const LedFaults& o) const { return !(*this == o); }
};

// MODE2 (01h) and EFLAG0..3 (46h..49h) as read, and where the lamps and the
// backlight strings sit among OUT0..15. Two bits per output: 01b short,
// 10b open; 00b and the undefined 11b are not reported.
LedFaults decode_led_faults(uint8_t mode2, const uint8_t eflag[4],
                            int lamp0, int lamps, int backlight0, int backlights);

struct Config {
    uint16_t base_rpm       = 6000;   // lamp 1 lights here
    uint16_t span_rpm       = 2400;   // lamp 8 lights at base + span
    uint16_t hysteresis_rpm = 120;    // a lit lamp stays lit until this far below
    uint16_t flash_rpm      = 8800;   // above this, all lamps flash
    // Predictive lamps: decide them for where the engine will be this far
    // ahead, at the rate it is climbing now. The rider sees a lamp and shifts
    // a reaction time later, 150-250 ms, and in a low gear at full throttle
    // the rpm climbs thousands per second meanwhile, so a lamp on time is a
    // shift late. 0 is off: the lamps show the rpm as it is.
    uint16_t lead_ms        = 0;
    // The most the lead may add. A rate that is noise, or a glitch at the
    // ceiling of RateEstimator, can bring the lamps forward this far and
    // no further.
    uint16_t lead_cap_rpm   = 1000;
    // Rate below this is treated as noise and leads nothing; above it, only
    // the excess leads, so nothing jumps where the floor is crossed. A
    // steady engine with a percent of period jitter reads up to ~1000 rpm/s
    // either way (test_tach), which at a 150 ms lead would wobble a lamp by
    // more than the hysteresis. BRINGUP: measure the real tach's jitter.
    uint16_t lead_floor_rpm_s = 1000;
    // Over flash_rpm the lamps blink, on for half of this, off for half.
    // 125 ms is 8 Hz: fast enough to read as a warning out of the corner of
    // an eye, slow enough that an LED off for 62 ms is plainly off.
    uint16_t flash_period_ms = 125;
};

// The rpm the lamps are decided on: `rpm`, plus (rpm_per_s - the floor)
// times lead_ms while the engine is climbing, at most lead_cap_rpm more. A steady or
// falling engine is shown as it is -- predicting a fall would put lamps out
// early, which tells the rider nothing.
uint16_t lamp_rpm(float rpm, float rpm_per_s, const Config& c);

// Threshold for one lamp, 0-indexed.
uint16_t threshold(const Config& c, int lamp);

// Returns an 8-bit mask, bit 0 = lamp 1. `prev` is last call's mask and is
// what makes the hysteresis work — pass 0 on the first call.
uint8_t pattern(uint16_t rpm, const Config& c, uint8_t prev);

// The rpm the engine got up to before a shift, held a while after it.
//
// It follows the rpm up; when the rpm falls away from it (a shift, a roll
// off) it stays where it was for `hold_ms`, then lets go and follows the rpm
// down until it climbs again. What is marked is only the held part, and only
// once the rpm is `gap_rpm` below it: in gear and climbing, the mark would
// sit on the needle and say nothing.
//
// One of these runs in `ctrl`, on the tach's displayed rpm, and its answer is
// published (Engine::rpm_peak, shift_hold) for the lamps and both tachs.
class PeakHold {
public:
    // Returns the rpm to mark, or 0 for none. `hold_ms` of 0 is off.
    float update(float rpm, uint32_t t_ms, uint32_t hold_ms, float gap_rpm = 400.0f);
    void reset();

private:
    float    peak_ = 0.0f;
    uint32_t at_ms_ = 0;       // when the rpm was last at the peak
    bool     released_ = true; // the hold has run out; following the rpm down
};

// The one lamp a held peak keeps lit: the highest lamp `peak_rpm` reaches,
// as a mask bit (lamp 8 above the flash), 0 below the first lamp.
uint8_t hold_lamp(uint16_t peak_rpm, const Config& c);

// True when RPM is past flash_rpm. The caller decides the blink rate, so
// this stays free of any notion of time.
bool over_limit(uint16_t rpm, const Config& c);

// The blink over the limit, for the lamps themselves. Over the limit every
// lamp is on for the first half of each period and off for the second,
// counted from when the rpm crossed, so the first thing the rider sees is
// all eight lit, never a dark bar. Under it, `mask` goes through untouched.
//
// `ctrl` runs one on every tick and edge and the sim runs the same one, so
// the window blinks when, and as fast as, the handlebar does.
class Flasher {
public:
    uint8_t apply(uint8_t mask, bool over, uint32_t t_ms, const Config& c);
    void reset() { over_ = false; }

private:
    bool     over_ = false;
    uint32_t since_ms_ = 0;    // when the rpm went over
};

}  // namespace core::shift
