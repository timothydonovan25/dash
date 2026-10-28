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

// One Heart Rate Measurement notification (GATT 0x2A37).
struct Measurement {
    uint16_t bpm = 0;
    bool     contact_known = false;   // the sensor reports skin contact
    bool     contact = false;
    uint8_t  rr_count = 0;            // RR intervals in this packet, kept up to 4
    uint16_t rr_ms[4] = {};
};

// False for a packet too short for what its flags say, or a bpm of 0.
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

// Heart-rate zone 1-5 by percent of the rider's maximum (50-60 % is zone 1,
// 90 % and over zone 5), 0 below zone 1 or with no maximum.
uint8_t zone(uint16_t bpm, uint16_t max_bpm);

}  // namespace core::hr
