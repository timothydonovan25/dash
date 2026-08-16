// Which gear the bike is in, worked out from rpm against road speed.
//
// There is no gear position sensor on a CRF250F. The ratio of engine speed
// to road speed is fixed per gear by the gearbox, the final drive and the
// rolling radius, so `rpm / mph` identifies the gear: take the ratio and
// show whichever gear it is closest to.
//
// That is the whole of it. There is deliberately no clutch detection, no
// slip rejection and no hold through a shift — the reading follows the
// ratio, and during a clutch pull it will move about, because that is what
// the ratio does. A rider reads this tile as a reminder of roughly where
// they are in the box, not as an instrument.
//
// The one thing it cannot do is divide by a road speed of zero, so there is
// a floor below which it reports nothing. Neutral comes from the switch on
// the gearbox and is reported separately, because that gear really is sensed.
//
// The ratios depend on the sprockets and the rolling radius, so they are
// configuration rather than constants. The defaults are a starting point for
// a stock bike and want calibrating: ride at a steady speed in each gear and
// read the ratio off the DIAG screens.
#pragma once
#include <cstdint>

namespace core::gear {

inline constexpr uint8_t kMaxGears = 6;

// 0 means "no answer" — stopped, or the engine not turning. Neutral is
// reported by the gearbox switch, not by this.
inline constexpr uint8_t kUnknown = 0;

struct Config {
    uint8_t count = 5;                  // the CRF250F is a five-speed

    // Engine rpm per mph of road speed, per gear, tallest gear last. 5th at
    // 130 puts 8500 rpm at about 65 mph, which is about what this will do.
    float rpm_per_mph[kMaxGears] = {420.0f, 290.0f, 215.0f, 165.0f, 130.0f, 0.0f};

    // Below either of these there is no ratio to speak of: one divides by
    // zero, the other means the engine is not turning and the number would
    // be a tall gear by arithmetic rather than by fact.
    float min_speed_mph = 3.0f;
    float min_rpm       = 500.0f;
};

// The measured ratio, or 0 when there is nothing to measure. Exposed so the
// DIAG screen can show it while the table is being calibrated.
float ratio(float rpm, float speed_mph, const Config& c);

// The gear whose ratio is closest to the measured one, or kUnknown when
// there is no ratio. Neutral short-circuits to kUnknown so the caller can
// show the sensed N instead.
uint8_t estimate(float rpm, float speed_mph, bool neutral, const Config& c);

}  // namespace core::gear
