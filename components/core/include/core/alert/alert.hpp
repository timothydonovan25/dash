// What is worth interrupting the rider for.
//
// A pure function of the Frame stream: it looks at the health table, the
// rails and the attitude, and says which single alert should be on the
// screen right now. The UI owns the overlay that draws it; this decides
// only *whether* and *which*.
//
// Three rules keep it from being a nuisance, and each is tested:
//   * A condition must hold for its onset time before it shows. A bump that
//     dips the battery reading for a frame is not a charging fault.
//   * It must be gone for a moment before it clears, so a reading that
//     flickers around its threshold does not strobe the overlay.
//   * An alert the rider has dismissed stays dismissed until its condition
//     has genuinely cleared. Nothing re-asserts itself after one press.
#pragma once
#include <cstdint>
#include "core/state/slots.hpp"

namespace core::alert {

// Declaration order is priority order: when several are active, the lowest
// shows, so the bike on its side outranks a dead shift lamp.
enum class Kind : uint8_t {
    None = 0,
    NoPulse,       // the rider's watch, worn this ride, says no pulse
    BikeDown,      // leaned right over and not moving
    BatteryHigh,   // regulator fault: a battery cooked at 16 V
    BatteryLow,    // engine running and not charging
    FrontWheel,    // the speed sensor has gone quiet
    RearWheel,
    GpsLost,       // mid-session, a lap timer with no position
    SdCard,        // the log has stopped
    ShiftLamps,    // the lamp driver is gone (and the backlight with it)
    Imu,           // lean, jump and lap-delta inputs are gone
    Service,       // the engine-hours interval has run out; last, it can wait
    Count
};

const char* text(Kind k);

// Plain data so the thresholds appear in one place and a test can shrink
// the times.
struct Config {
    float    down_lean_deg  = 70.0f;
    float    down_mph       = 3.0f;
    float    batt_low_v     = 11.8f;
    float    batt_high_v    = 16.0f;
    float    charging_rpm   = 2500.0f;   // below this a low reading is just idle
    uint32_t clear_ms       = 1500;
};

class Alerter {
public:
    explicit Alerter(const Config& c = Config{}) : cfg_(c) {}

    // Call with every Frame. The alert to show, or Kind::None.
    Kind update(const state::Frame& f);

    // The rider dismissed whatever update() last returned.
    void acknowledge();

    Kind shown() const { return shown_; }

private:
    static constexpr uint8_t kN = static_cast<uint8_t>(Kind::Count);

    bool condition(Kind k, const state::Frame& f) const;
    static uint32_t onset_ms(Kind k);

    Config   cfg_;
    // Per kind: when the condition began (or stopped), and the two states.
    uint32_t since_[kN]  = {};
    bool     pending_[kN] = {};   // condition true, waiting out its onset
    bool     active_[kN]  = {};
    bool     gone_[kN]    = {};   // active, and the condition has dropped
    bool     acked_[kN]   = {};
    Kind     shown_ = Kind::None;
};

}  // namespace core::alert
