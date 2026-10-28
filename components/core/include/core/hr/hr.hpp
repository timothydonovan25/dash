// Heart rate from a watch or a chest strap over the standard Bluetooth Heart
// Rate Service (0x180D), as a Garmin watch sends it with Broadcast Heart Rate
// on. The dash is the central: it scans, connects, and subscribes to Heart
// Rate Measurement (0x2A37). `ble` does the radio; this is everything it
// decides, so it is host-tested.
//
// Which sensor: the strongest Heart Rate advertiser above a floor, once a
// scan has had time to hear them all. A watch on the rider's wrist is an arm's
// length from the dash, so at a track day with other people's straps about,
// the nearest is the rider's. Once one has connected it is the one for the
// power-on: a dropout reconnects to it, not to whoever is strongest then. It is
// matched by name as well as by address, because a watch may rotate a private
// address. Turning the setting off and on forgets it.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/state/vehicle.hpp"

namespace core::hr {

inline constexpr uint16_t kServiceUuid = 0x180D;
inline constexpr uint16_t kMeasurementUuid = 0x2A37;
inline constexpr uint16_t kCccdUuid = 0x2902;

// How long a scan listens before choosing. With nobody heard the next scan
// starts at once (the search is continuous while Watch HR is on); after a
// scan, connect or subscribe that failed, it waits kRescanMs.
inline constexpr uint32_t kScanMs = 3000;
inline constexpr uint32_t kRescanMs = 5000;
// Below this a sensor is someone else's. A wrist at arm's length reads
// about -50 to -70 dBm, through a jacket sleeve worse.
inline constexpr int8_t kRssiFloorDbm = -85;
// A reading older than this is not shown or logged. A watch sends about one
// a second.
inline constexpr uint32_t kStaleMs = 5000;
// Connected and subscribed but silent this long: the broadcast was stopped
// on the watch without it dropping the link. Let it go and look again.
inline constexpr uint32_t kSilentDropMs = 20000;
// How long the watch must say "no pulse" before the NO PULSE alert. A Garmin
// takes 2-3 s to notice it is off the wrist, and a wrist shifting on rough
// ground can lose contact for a moment.
inline constexpr uint32_t kNoPulseMs = 10000;

// One Heart Rate Measurement notification (GATT 0x2A37).
struct Measurement {
    uint16_t bpm = 0;
    bool     contact_known = false;   // the sensor reports skin contact
    bool     contact = false;
    uint8_t  rr_count = 0;            // RR intervals in this packet, kept up to 4
    uint16_t rr_ms[4] = {};

    // Whether this reading is a heartbeat. Off the wrist a Garmin keeps
    // sending, twice a second, the last bpm it had with the contact bit
    // clear (seen: 06 61 worn, then 04 5E for as long as it was off), so the
    // number alone would freeze on the screen.
    bool pulse() const { return bpm != 0 && (!contact_known || contact); }
};

// False for a packet too short for what its flags say. A bpm of 0 is a
// reading, with no pulse in it.
bool parse(const uint8_t* p, size_t n, Measurement& out);

// Whether an advertisement (the AD structures of an advertising packet or a
// scan response) lists the Heart Rate Service, and its name if it has one.
// `name` gets at most name_len-1 characters and is always terminated.
struct Advert {
    bool hrs = false;
    char name[20] = {};
};
Advert parse_advert(const uint8_t* ad, size_t n);

struct Addr {
    uint8_t type = 0;
    uint8_t val[6] = {};
};
bool same(const Addr& a, const Addr& b);

// Chooses whom to connect to from what a scan heard.
class Picker {
public:
    // A new scan: forget the last one's candidates, keep the latch.
    void begin_scan();
    // One advertisement, or its scan response. Only Heart Rate advertisers
    // should be passed; a scan response, which carries the name but not
    // always the service, is the same device heard again.
    void heard(const Addr& a, int8_t rssi_dbm, const char* name);
    // After the scan: the sensor to connect to, false for none, and its
    // name ("" if it never said).
    bool choice(Addr& out) const;
    const char* choice_name() const { return best_name_; }
    // A connection to `a` worked: it is the sensor for this power-on.
    void latch(const Addr& a, const char* name);
    // Forget the latched sensor (the setting went off).
    void reset();
    bool latched() const { return latched_; }
    const char* name() const { return name_; }

private:
    bool matches_latch(const Addr& a, const char* name) const;
    bool   latched_ = false;
    Addr   latch_{};
    char   name_[20] = {};
    bool   have_ = false;
    Addr   best_{};
    int8_t best_rssi_ = -128;
    char   best_name_[20] = {};
};

// The power-on's heart rate: average over the time readings were coming,
// highest, lowest. Average is per reading, and a watch sends one a second.
class Session {
public:
    void add(uint16_t bpm);
    uint16_t max_bpm() const { return max_; }
    uint16_t min_bpm() const { return min_; }
    uint16_t avg_bpm() const;
    uint32_t count() const { return count_; }

private:
    uint32_t count_ = 0;
    uint64_t sum_ = 0;
    uint16_t max_ = 0;
    uint16_t min_ = 0;
};

// Whether the slot's reading is current enough to show or log.
bool fresh(const state::Heart& h, uint32_t now_ms);

// The watch is connected, has had a pulse on this link, and its current
// readings say there is none: taken off, or worse. Readings must still be
// arriving: a watch that has gone quiet or out of range says nothing about
// the rider. The alert waits kNoPulseMs of this.
bool no_pulse(const state::Heart& h, uint32_t now_ms);

// The last ten minutes, a sample every 2 s, for STAT -> HEART's graph. Fed
// every Frame by ui::App, which outlives the screen; a sample is the reading
// at its step if it is fresh and has a pulse, else 0 (a gap in the line).
// Steps no Frame arrived for are gaps too.
class History {
public:
    static constexpr size_t   kSamples = 300;
    static constexpr uint32_t kStepMs  = 2000;

    void update(const state::Heart& h, uint32_t now_ms);
    // 0 the oldest, kSamples - 1 the newest; 0 for no reading.
    uint8_t at(size_t i) const { return s_[(head_ + i) % kSamples]; }
    // Moves with every sample: what a graph compares to know it must redraw.
    uint32_t seq() const { return seq_; }
    // Lowest and highest reading held; false for none.
    bool range(uint8_t& lo, uint8_t& hi) const;

private:
    void push(uint8_t v);
    uint8_t  s_[kSamples] = {};
    size_t   head_ = 0;      // the oldest, and the next written
    uint32_t step_ = 0;
    bool     started_ = false;
    uint32_t seq_ = 0;
};

// Heart-rate recovery: the pulse when the bike stopped, less the pulse a
// minute later (and two, if it stays parked), the usual fitness measure.
//
// Parked only. Tight technical riding stops all the time, feet down and in
// gear, and its pauses are not rest: a stop counts only if within
// kParkGraceMs the bike is in neutral or the engine is off, and it stays
// stopped and that way for the minute. It must follow kRideMinMs of riding
// (above kRideMph) since the last parked minute, so pushing the bike across
// the pits is not a ride. A missing pulse at either end, or moving off,
// gives no result.
inline constexpr float    kStopMph     = 1.0f;
inline constexpr float    kRideMph     = 5.0f;
inline constexpr uint32_t kRideMinMs   = 120000;
inline constexpr uint32_t kParkGraceMs = 15000;
inline constexpr uint32_t kRecoveryMs  = 60000;

struct RecoveryInput {
    uint32_t t_ms = 0;
    float    mph = 0.0f;
    bool     speed_ok = false;    // no speed, no telling it is parked
    bool     neutral = false;
    bool     engine_running = false;
    uint16_t bpm = 0;             // a fresh reading with a pulse, else 0
};

class Recovery {
public:
    // Every logic pass. True when a result lands (one minute or two).
    bool update(const RecoveryInput& in);
    const state::HrRecovery& publish() const { return out_; }

private:
    enum class Phase : uint8_t { Idle, Stopped, Measuring };
    void end();
    Phase    phase_ = Phase::Idle;
    bool     was_moving_ = false;
    bool     have_t_ = false;
    uint32_t last_t_ = 0;
    uint32_t ride_ms_ = 0;
    bool     have1_ = false;
    state::HrRecovery out_{};
};

// Heart-rate zone 1-5 by percent of the rider's maximum (50-60 % is zone 1,
// 90 % and over zone 5), 0 below zone 1 or with no maximum.
uint8_t zone(uint16_t bpm, uint16_t max_bpm);

}  // namespace core::hr
