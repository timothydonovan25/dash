// Tyre pressure from BLE TPMS sensors (§11.2). They never connect: each one
// puts its pressure, temperature, battery and an ID in the manufacturer
// data of an advertisement. `ble` scans and hands every advert here; this
// decodes it, keeps the two wheels, and does the CONFIG -> TPMS learn. Pure,
// so the decoding is host-tested against captured bytes.
//
// One family is known so far (decode_0100). Wistek's own layout is a guess
// until a sensor has been sniffed (console `tpms sniff`): when it turns out
// to be something else, it is one more decoder in tpms.cpp and one more test.
//
// A sensor's ID is 24 bits, the low three bytes of what the sensor says it
// is. 24, not 32, because a ConfigSet carries a float, and a float holds a
// whole number exactly only to 2^24.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/state/vehicle.hpp"

namespace core::tpms {

inline constexpr uint32_t kIdMask = 0xFFFFFF;
// A wheel not heard from in this long reads `--`. A parked sensor sleeps
// and sends rarely, a rolling one every few seconds.
inline constexpr uint32_t kStaleMs = 10u * 60u * 1000u;
// How long a learn waits for a new sensor.
inline constexpr uint32_t kLearnMs = 60000;

struct Reading {
    uint32_t id = 0;        // 24 bits; 0 is never a sensor
    float    psi = 0.0f;    // gauge
    float    temp_c = 0.0f;
    uint8_t  batt_pct = 0;
    bool     alarm = false;
};

// The manufacturer data (company ID first, as on air) of the family with
// company ID 0x0100: 18 bytes, the sensor's 6-byte address (first byte
// 0x80 + its number), pressure in Pa and temperature in 0.01 °C as LE
// int32s, battery %, alarm. The layout the `TPMSII` app's sensors use.
bool decode_0100(const uint8_t* mfg, size_t n, Reading& out);

// The advertisement as received (AD structures): its manufacturer data
// through every known decoder. False when no decoder takes it.
bool decode(const uint8_t* ad, size_t n, Reading& out);

// The manufacturer-data field of an advertisement, or nullptr.
const uint8_t* find_mfg(const uint8_t* ad, size_t n, size_t& mfg_len);

enum class Wheel : uint8_t { Front = 0, Rear = 1 };

// The two wheels. `ids` are CONFIG's (Settings::tpms_front_id / _rear_id,
// 0 unbound), passed in each call so the learned binding has one writer,
// `logic`, like every setting.
class Tracker {
public:
    // CONFIG -> TPMS -> Learn front / rear: the next sensor heard that is
    // not the other wheel's becomes this wheel's.
    void learn(Wheel w, uint32_t now_ms);
    bool learning() const { return learning_; }
    Wheel learning_wheel() const { return learn_wheel_; }

    struct Learned { bool any = false; Wheel wheel = Wheel::Front; uint32_t id = 0; };

    // A decoded reading. Returns what a learn bound, for `ble` to post as a
    // ConfigSet; the reading counts for that wheel at once.
    Learned on_reading(const Reading& r, const uint32_t ids[2], uint32_t now_ms);

    // Ages the wheels and drops a stale one, or a learn that timed out.
    // True when what publish() returns changed in a way a screen shows.
    bool tick(const uint32_t ids[2], uint32_t now_ms);

    const state::Tpms& publish() const { return out_; }

private:
    state::Tpms out_;
    uint32_t last_ms_[2] = {};
    uint32_t last_id_[2] = {};   // whose reading out_ holds
    // A learned ID until its ConfigSet comes back through `ids`: until then
    // `ids` still holds what it was (`ids_then_`). Any change to it ends this.
    uint32_t pending_[2] = {};
    uint32_t ids_then_[2] = {};
    void bound(const uint32_t ids[2], uint32_t out[2]);
    bool     learning_ = false;
    Wheel    learn_wheel_ = Wheel::Front;
    uint32_t learn_until_ = 0;
};

}  // namespace core::tpms
