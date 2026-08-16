// Which gear the bike is in, worked out from rpm against speed.
//
// There is no gear position sensor on a CRF250F, so this is an inference,
// and the honest version of an inference says when it does not know. The
// ratio of engine speed to road speed is fixed per gear by the gearbox, the
// final drive and the rolling radius — so `rpm / mph` identifies the gear,
// as long as the clutch is engaged and the bike is actually moving.
//
// Three cases where it cannot know, and must say so rather than guess:
//
//   - **The clutch is in.** rpm and speed are decoupled, so the ratio is
//     whatever the rider's left hand says it is. This is exactly the moment
//     a gear indicator is being looked at, which is why it holds its last
//     reading briefly rather than flickering to nothing mid-shift.
//   - **Stopped, or nearly.** Dividing by a speed near zero gives a ratio
//     near infinity, and one that swings wildly for a tenth of a mile an
//     hour of GPS noise.
//   - **Between ratios.** A reading that matches no gear within tolerance is
//     a slipping clutch, a wheelspin, or a mis-calibrated table. None of
//     those is a gear.
//
// The ratios themselves depend on the sprockets and the rolling radius, so
// they are configuration, not constants. The defaults are a starting point
// for a stock CRF250F and want calibrating: ride at a steady speed in each
// gear and read `rpm / mph` off the DIAG screens.
#pragma once
#include <cstdint>

namespace core::gear {

inline constexpr uint8_t kMaxGears = 6;

// 0 means "not known". Neutral is reported separately, by the switch on the
// gearbox, which is the one gear this bike does sense directly.
inline constexpr uint8_t kUnknown = 0;

struct Config {
    uint8_t count = 5;                  // the CRF250F is a five-speed

    // Engine rpm per mph of road speed, per gear. Starting estimates for a
    // stock bike; 5th at 130 puts 8500 rpm at about 65 mph, which is about
    // right for what this thing will do.
    float rpm_per_mph[kMaxGears] = {420.0f, 290.0f, 215.0f, 165.0f, 130.0f, 0.0f};

    // How far off a gear's ratio a reading may be and still count as that
    // gear.
    //
    // The ceiling is set by the closest pair in the table. Fourth and fifth
    // are 27% apart, and two bands of +/-t percent stop overlapping only
    // below about 11.9% — so 10 leaves a clean gap everywhere and still
    // tolerates a table that was calibrated roughly. Widen it and a single
    // ratio starts matching two gears at once, which makes the reading
    // depend on the order of the table rather than on the bike.
    // test_gear checks this against whatever the table says.
    float tolerance_pct = 10.0f;

    // Below either of these the ratio is noise.
    float min_speed_mph = 5.0f;
    float min_rpm       = 900.0f;

    // How long to keep showing the last gear when the reading stops making
    // sense. One clutch pull and shift is a few hundred milliseconds.
    uint32_t hold_ms = 600;
};

class Estimator {
public:
    // Call at whatever rate the caller has; time is only used for the hold.
    // Returns the gear, or kUnknown.
    uint8_t update(float rpm, float speed_mph, bool neutral, uint32_t t_ms,
                   const Config& c);

    uint8_t gear() const { return gear_; }
    void reset();

    // The measured ratio, for calibrating the table on the DIAG screen.
    // Zero when there is nothing sensible to measure.
    float last_ratio() const { return ratio_; }

private:
    uint8_t  gear_ = kUnknown;
    float    ratio_ = 0.0f;
    uint32_t last_good_ms_ = 0;
    bool     started_ = false;
};

}  // namespace core::gear
