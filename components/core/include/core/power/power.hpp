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

}  // namespace core::power
