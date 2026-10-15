// A bike, for the simulator to pretend to be.
//
// This is the only fake in the whole thing. Everything downstream of it —
// the lap timer, the attitude filter, the shift pattern, every widget and
// every screen — is the firmware's own code, so what the window shows is
// what the panel will show.
#pragma once
#include "core/battery/battery.hpp"
#include <cstdint>
#include "core/jump/jump.hpp"
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
    // Each wheel against road speed, percent: +30 is spinning 30% fast,
    // -100 is locked. GPS keeps telling the truth, as it would.
    float front_slip_pct = 0.0f;
    float rear_slip_pct  = 0.0f;
    // A wheel sensor that has died: it reads 0 whatever the bike does,
    // which core::fusion::WheelCheck faults after 5 s at speed.
    bool  front_dead = false;
    bool  rear_dead  = false;
    bool  neutral    = false;
    bool  ecu_online = false;   // NullEngine ships, so this is off by default
    bool  gps_fix    = true;
    int   sats       = 11;

    // The simulator has no radio, so nothing here plays the phone's part of
    // §11.1. The front end fills the media slot from whatever is actually playing on
    // the machine running the simulator, which is the closest thing to a
    // real phone available before the radio lands -- and real track names
    // are the ones that turn out not to fit.
    //
    // An empty title means nothing is playing, and the block disappears.
    char  media_title [60] = {};
    char  media_album [60] = {};
    char  media_artist[60] = {};
    // §11.1 tag `Y`. Real phone never sends a whole file, so neither does
    // this: the front end sets whatever one line it wants shown.
    char  media_lyric [60] = {};
    // Total track length; 0 means unknown, same as a real §11.1 phone that
    // hasn't said.
    uint32_t media_duration_ms = 0;
    // §11.1 `P` and `S`, as the front end reads them off the local player:
    // `media_position_ms` was true at sim time `media_position_at_ms`, and
    // the screens extrapolate from there while playing, exactly as they do
    // from a phone. Until a position arrives `Bike::step` advances one on
    // its own from the title change, so a scripted run with no player still
    // has a ring that moves.
    bool     media_position_known = false;
    uint32_t media_position_ms    = 0;
    uint32_t media_position_at_ms = 0;
    uint8_t  media_state          = 1;   // 0 stop 1 play 2 pause

    // Milliseconds since UTC midnight, from the host clock, so the dash
    // clock reads the time of day rather than how long the sim has run.
    uint32_t utc_ms = 0;

    // Drives the bike round a synthetic circuit at `speed_mph`, so the lap
    // timer, the delta matcher and the map screens have something real to
    // chew on. Lean is then computed from the corner rather than set.
    bool  auto_ride  = false;

    // Filled in by the sim loop from core::shift::pattern(), exactly as
    // `ctrl` does before it publishes.
    uint8_t shift_mask  = 0;
    bool    shift_flash = false;
    uint8_t gear        = 0;
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

    // Send it off a jump. The accelerometer goes to zero for the duration
    // and then spikes, and the firmware's own core::jump::Detector works out
    // the rest — this does not tell the dash it jumped, it makes the sensor
    // say what a sensor says during a jump.
    void trigger_jump(uint32_t now_ms, uint32_t airtime_ms = 900);

    // A slip that comes and goes, which a slider cannot do: real spins and
    // locks last a fraction of a second, and the bar's 0.7 s peak hold is
    // for exactly that. Played on top of the slip sliders.
    enum class Slip : uint8_t { Spin, FrontLock, RearLock, Count };
    void trigger_slip(Slip kind, uint32_t now_ms);

    // Where the bike is on the circuit, 0..1. The front end draws a dot.
    float lap_fraction() const { return f_; }
    // Where the bike's speed actually is, chasing the slider.
    float speed_mph() const { return speed_mph_; }
    double lat() const { return lat_; }
    double lon() const { return lon_; }

private:
    float  f_ = 0.0f;            // fraction round the circuit
    double lat_ = 42.500000;
    double lon_ = -83.100000;
    float  heading_deg_ = 0.0f;
    float  lean_deg_ = 0.0f;

    float    speed_mph_ = 0.0f;      // the bike's, ramping to the slider
    float    accel_g_ = 0.0f;        // low-passed, from the speed
    float    prev_speed_mph_ = 0.0f;
    uint32_t air_until_ms_ = 0;
    // When each slip event started; 0 is none running.
    uint32_t slip_at_ms_[static_cast<uint8_t>(Slip::Count)] = {};
    uint32_t spike_until_ms_ = 0;

    core::jump::Detector jump_;
    core::battery::Monitor battery_;   // as `power` runs it, on the slider's VIN

    uint32_t media_position_ms_ = 0;
    char     media_title_prev_[60] = {};
};

}  // namespace sim
