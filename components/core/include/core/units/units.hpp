// Unit conversion and display formatting, in one place.
//
// The firmware stores one canonical unit per quantity — mph, feet, miles,
// Celsius, psi, gallons — and converts only when a value is about to be
// drawn. Nothing but this file and the screens know the user's preference.
//
// Formatting writes into a caller-supplied buffer. There is no allocation
// after boot, so nothing here returns a std::string.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::units {

enum class System : uint8_t { Imperial = 0, Metric };

// --- conversions --------------------------------------------------------
// Named from → to, so a call site reads as a sentence and cannot be
// inverted by accident.

constexpr float kFtPerMile   = 5280.0f;
constexpr float kFtPerMeter  = 3.280839895f;
constexpr float kMphPerFtPerS= 0.681818182f;     // 1 ft/s = 0.6818 mph
constexpr float kKmhPerMph   = 1.609344f;
constexpr float kPsiPerBar   = 14.5037738f;
constexpr float kLitrePerGal = 3.785411784f;

constexpr float kmh_from_mph(float mph)   { return mph * kKmhPerMph; }
constexpr float mph_from_kmh(float kmh)   { return kmh / kKmhPerMph; }
constexpr float mph_from_ftps(float ftps) { return ftps * kMphPerFtPerS; }
constexpr float ftps_from_mph(float mph)  { return mph / kMphPerFtPerS; }

constexpr float m_from_ft(float ft)       { return ft / kFtPerMeter; }
constexpr float ft_from_m(float m)        { return m * kFtPerMeter; }
constexpr float mi_from_ft(float ft)      { return ft / kFtPerMile; }
constexpr float ft_from_mi(float mi)      { return mi * kFtPerMile; }
constexpr float km_from_mi(float mi)      { return mi * kKmhPerMph; }
constexpr float mi_from_km(float km)      { return km / kKmhPerMph; }

constexpr float f_from_c(float c)         { return c * 1.8f + 32.0f; }
constexpr float c_from_f(float f)         { return (f - 32.0f) / 1.8f; }

constexpr float bar_from_psi(float psi)   { return psi / kPsiPerBar; }
constexpr float psi_from_bar(float bar)   { return bar * kPsiPerBar; }

constexpr float l_from_gal(float gal)     { return gal * kLitrePerGal; }
constexpr float gal_from_l(float l)       { return l / kLitrePerGal; }

// --- display ------------------------------------------------------------
// Each returns the number in the user's system and, through `unit`, the
// label to draw next to it. The label is a literal with static lifetime.

float speed_for_display(float mph, System s, const char** unit);
float distance_for_display(float mi, System s, const char** unit);
float temp_for_display(float c, System s, const char** unit);
float pressure_for_display(float psi, System s, const char** unit);
float volume_for_display(float gal, System s, const char** unit);

// --- text ---------------------------------------------------------------

// Fixed-point number into `buf`. Returns the length written, never more
// than n-1 characters plus a terminator.
size_t fmt_float(char* buf, size_t n, float v, int decimals);

// The §12 rule in one function: a reading from a device that is not Ok
// renders as `--`, never as a stale value and never as zero. Every widget
// that shows a sensor number goes through this.
size_t fmt_value(char* buf, size_t n, float v, int decimals, bool valid);

// Lap and split times. Under an hour: `1:23.45`. Over: `1:02:03.4`.
// `ms == 0` with `valid == false` gives `--:--.--`, which is what an unset
// best lap must look like.
size_t fmt_duration(char* buf, size_t n, uint32_t ms, bool valid);

// A signed delta against a reference lap: `+0.4`, `-1.1`, or `--`.
//
// One decimal by default, and that is not a display preference. The relative
// error between two laps recorded minutes apart does not fully cancel, so a
// tenth is about the honest resolution of a GPS-matched delta — `+0.02` on a
// dashboard claims a precision nobody has. Comparing two measured lap times
// against each other is a different question and may ask for two.
size_t fmt_delta(char* buf, size_t n, int32_t ms, bool valid, int decimals = 1);

// Clock time from a UTC millisecond count, with a whole-hour offset for the
// local zone. `24:00` wrap handled; no date, because the dash never shows one.
size_t fmt_clock(char* buf, size_t n, uint32_t utc_ms, int tz_offset_hours,
                 bool valid);

}  // namespace core::units
