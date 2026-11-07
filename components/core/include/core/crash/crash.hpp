// Crash detection and the SOS text (features §35).
//
// A crash is three things in order, and none of them alone: the bike was
// moving, something hit it hard, and then it lay on its side, still, for a
// few seconds. A jump landing is the hit without the lying down; a tip-over
// in the car park is the lying down without the speed; the board face-down
// on a car seat is neither. Then a countdown the rider can cancel with any
// button, and only then the phone is asked to text someone.
//
// `logic` runs the Detector at 50 Hz and is its one writer; `ble` reads the
// published Crash and sends the Ctrl lines below until the phone answers.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::crash {

// The largest |a| in a short window, from every IMU sample (200 Hz), so a
// 10 ms spike is not lost between logic's 50 Hz looks. `imu` publishes
// hit_g/hit_ms into Attitude; the detector sees each peak once by its time.
class PeakHold {
public:
    static constexpr uint32_t kWindowMs = 500;
    void update(float mag_g, uint32_t t_ms) {
        if (mag_g >= g_ || t_ms - ms_ > kWindowMs) { g_ = mag_g; ms_ = t_ms; }
    }
    float    g() const  { return g_; }
    uint32_t ms() const { return ms_; }
private:
    float    g_ = 0.0f;
    uint32_t ms_ = 0;
};

struct Config {
    float    impact_g      = 3.0f;    // over this is a hit
    float    moving_mph    = 10.0f;   // the bike was ridden upright, not pushed
    uint32_t moving_ms     = 5000;    // ...this recently before the hit
    float    down_tilt_deg = 60.0f;   // on its side: gravity this far off the frame's up
    float    still_mph     = 3.0f;
    uint32_t down_ms       = 5000;    // down and still this long
    uint32_t after_hit_ms  = 30000;   // and the lying down began this soon after the hit
    uint32_t countdown_ms  = 30000;   // then this long to cancel
};

enum class Phase : uint8_t {
    Idle = 0,
    Countdown,   // a crash; any button cancels
    Sent,        // the SOS is out (or going out); E says "I'm OK"
};

struct Input {
    uint32_t t_ms = 0;
    bool     enabled = false;   // CONFIG -> Phone -> Crash SOS
    bool     imu_ok = false;
    float    mph = 0.0f;        // the speedo's best speed
    float    hit_g = 0.0f;      // Attitude::hit_g / hit_ms
    uint32_t hit_ms = 0;
    // The low-passed gravity vector in the frame's axes (Attitude g_*).
    float    g_lat = 0.0f, g_lon = 0.0f, g_vert = 1.0f;
};

// What `logic` publishes (Slots::crash).
struct State {
    Phase    phase = Phase::Idle;
    uint8_t  countdown_s = 0;   // whole seconds left, rounded up
    // Each SOS has a number, so the phone can tell a resend from a new one;
    // `ok_id` is the SOS the rider has since said was a false alarm.
    uint16_t sos_id = 0;
    uint16_t ok_id = 0;
};

class Detector {
public:
    State update(const Input& in, const Config& c = Config{});
    // Any button during the countdown: never sent. E after it: "I'm OK".
    void cancel();
    // Console `crash test`: the countdown from now, as a real crash would
    // start it, so the phone and the text can be tried on the bench.
    void test(uint32_t now_ms, const Config& c = Config{});
    const State& state() const { return st_; }

private:
    State    st_;
    uint32_t last_moving_ms_ = 0;
    bool     have_moving_ = false;
    uint32_t armed_hit_ms_ = 0;     // a hit that followed riding; 0 none
    bool     armed_ = false;
    uint32_t seen_hit_ms_ = 0;
    uint32_t down_since_ms_ = 0;
    bool     down_ = false;
    uint32_t countdown_end_ms_ = 0;
};

// The phone's answer to an SOS, Feed tag `K` (§11.1).
enum class Ack : uint8_t {
    None = 0,      // nothing heard yet
    Texted = '1',  // the text went (the radio said so)
    Off = '0',     // the phone has Crash SOS off, or no contact set
    Failed = '2',  // tried and the network refused; the dash sends again
};

// Ctrl lines (dash -> phone), §11.1. `X <id> <lat> <lon> <age_s>`: an SOS,
// `age_s` the fix's age in seconds, -1 with no fix ever. `X <id> ok`: that
// SOS was a false alarm. Returns the length, 0 if it did not fit.
size_t sos_line(uint16_t id, bool have_fix, double lat, double lon, int32_t age_s,
                char* out, size_t n);
size_t ok_line(uint16_t id, char* out, size_t n);

// `K <id> <code>`, from the phone. True and the two fields if it is one.
bool parse_ack(const uint8_t* data, size_t len, uint16_t& id, Ack& ack);

}  // namespace core::crash
