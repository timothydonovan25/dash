// Where the power is coming from, from one ADC reading.
//
// The P4 board divides VIN down to an ADC pin. VIN is the bike's 12 V
// through the fuse and reverse diode, or USB's 5 V through a Schottky when
// the bike is off the board. The two are far enough apart that one
// threshold tells them apart, and that is the whole point of the divider:
// before it, the firmware had to guess "bench" from "engine not turning",
// which is also true of a bike parked with the ignition on (dash-v1 netlist
// review §3).
//
// The arithmetic is here rather than in the driver so it can be tested on
// the host. The driver reports the millivolts at the pin and nothing else.
#pragma once
#include <cstdint>

namespace core::power {

struct Divider {
    float top_kohm    = 100.0f;   // VIN to the pin
    float bottom_kohm = 22.1f;    // pin to GND
};

// VIN in volts from the voltage at the ADC pin.
float vin_from_pin_mv(uint32_t pin_mv, const Divider& d);

enum class Source : uint8_t {
    Unknown = 0,   // no reading, or a reading too low to be either supply
    Usb,           // about 4.5 V: USB through the Schottky
    Bike,          // 12 V class
};

struct Thresholds {
    // Below this nothing is supplying VIN that the divider can see: an
    // unfitted divider, a dead ADC, or a pin that is not connected.
    float present_v = 3.0f;
    // USB arrives at about 4.5 V, a flat 12 V battery sits above 11 V, and
    // the buck stops at its 3.6 V UVLO long before VIN gets anywhere near
    // this line from either side.
    float bike_v = 8.0f;
};

Source classify(float vin_v, const Thresholds& t = Thresholds{});

// The shift lamps' master duty, in percent: the rider's brightness setting,
// but never above `usb_cap_pct` unless the supply is the bike. The cap is a
// current limit, not a preference, so the setting can lower it and never
// raise it.
uint8_t lamp_master_pct(bool on_bike, uint8_t setting_pct, uint8_t usb_cap_pct);

// Has the bike's supply just been cut?
//
// The bulk capacitance on VIN lasts about 9 ms at full load (docs/p4-board-
// pin-map.md), 17 ms with the backlight and lamps shed. That is enough to
// stop writing to the card and save the odometer; it is not enough to
// flush a file. So this is a trigger and nothing more: fed one VIN sample
// per millisecond, it says "Tripped" once, as early as it can be sure.
//
// It only ever fires after the dash has seen a bike supply, so a dash on
// USB (4.5 V, always) never trips, and a cranking dip that stays above
// `trip_v` does not either. If it does trip on a dip that then recovers,
// `Recovered` hands everything back: the cost of a false alarm is a second
// without a backlight, not a lost ride.
class FailDetector {
public:
    struct Config {
        float    arm_v   = 10.5f;   // seen this long enough => on the bike
        float    trip_v  = 9.0f;    // below this, for `trip_n`, is a cut
        float    ok_v    = 10.5f;   // back above this, for `ok_n`, is a recovery
        uint16_t arm_n   = 20;      // samples
        uint16_t trip_n  = 2;
        uint16_t ok_n    = 50;
        // Still alive this long after a trip, with the rail low and steady,
        // means USB is holding the board up. Let go and carry on.
        uint16_t usb_n   = 1000;
    };
    enum class Event : uint8_t { None, Tripped, Recovered };

    FailDetector() = default;
    explicit FailDetector(const Config& c) : cfg_(c) {}

    // One sample, at a steady rate.
    Event feed(float vin_v);

    bool tripped() const { return state_ == State::Tripped; }
    bool armed() const { return state_ == State::Armed; }

    // While tripped: samples in a row that the rail has stayed below
    // `trip_v`, counting the ones that tripped it. A real cut only goes on
    // falling; a glitch at speed comes back. `power` waits on this before a
    // flash write it would otherwise make with the bike moving.
    uint16_t low_run() const { return state_ == State::Tripped ? low_run_ : 0; }

private:
    enum class State : uint8_t { Idle, Armed, Tripped };
    Config   cfg_;
    State    state_ = State::Idle;
    uint16_t run_ = 0;       // consecutive samples of whatever is being waited on
    uint16_t since_trip_ = 0;
    uint16_t low_run_ = 0;
};

}  // namespace core::power
