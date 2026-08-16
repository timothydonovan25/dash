// A bike, for the simulator to pretend to be.
//
// This is the only fake in the whole thing. Everything downstream of it —
// the lap timer, the attitude filter, the shift pattern, every widget and
// every screen — is the firmware's own code, so what the window shows is
// what the panel will show.
#pragma once
#include <cstdint>
#include "core/state/slots.hpp"

namespace sim {

// What the sliders drive.
struct Controls {
    float speed_mph  = 0.0f;
    float rpm        = 0.0f;
    float lean_deg   = 0.0f;
    float pitch_deg  = 0.0f;
    float tps_pct    = 0.0f;
    float coolant_c  = 82.0f;
    float vbat       = 12.6f;
    bool  neutral    = false;
    bool  ecu_online = false;   // NullEngine ships, so this is off by default
    bool  gps_fix    = true;
    int   sats       = 11;

    // Drives the bike round a synthetic circuit at `speed_mph`, so the lap
    // timer, the delta matcher and the map screens have something real to
    // chew on. Lean is then computed from the corner rather than set.
    bool  auto_ride  = false;

    // Filled in by the sim loop from core::shift::pattern(), exactly as
    // `ctrl` does before it publishes.
    uint8_t shift_mask  = 0;
    bool    shift_flash = false;
};

// The circuit: a rounded rectangle, roughly a kilometre round, laid out at
// 42N so the flat-earth approximation in core/geo behaves as it does on the
// bikes' actual continent.
class Bike {
public:
    void reset();

    // Advance by `dt_ms` and publish Engine, Motion, Attitude and Gnss into
    // the slots, exactly as ctrl, imu and gnss would.
    void step(core::state::Slots& slots, const Controls& c, uint32_t now_ms,
              uint32_t dt_ms);

    // Where the bike is on the circuit, 0..1. The front end draws a dot.
    float lap_fraction() const { return f_; }
    double lat() const { return lat_; }
    double lon() const { return lon_; }

private:
    float  f_ = 0.0f;            // fraction round the circuit
    double lat_ = 42.500000;
    double lon_ = -83.100000;
    float  heading_deg_ = 0.0f;
    float  lean_deg_ = 0.0f;
};

}  // namespace sim
