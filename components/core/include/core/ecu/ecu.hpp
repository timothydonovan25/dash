// The K-line adapter's frames: adapter C6 -> dash C6 over ESP-NOW, then to
// the P4 (architecture §10.1, kline-adapter/review.md §10). This is the
// frame format, the decode of the ECU's live-data table and the staleness
// rule; the radio on both ends is the thin part and is not here.
//
// The adapter forwards the ECU's table bytes as it read them and the dash
// decodes. The byte map below is community reverse-engineering from other
// Honda/Keihin bikes (CRF250L, CBR600RR, Winner 150), not a CRF250F capture,
// and the dash is the end that can be updated from a card.
//
// One frame, at most one ECU table:
//
//   0  magic    0x4B
//   1  version  1
//   2  seq      +1 per frame, wraps
//   3  flags    bit 0: the ECU is answering
//   4  table    the Keihin table number (0x11 live data), 0 with len 0
//   5  len      payload bytes that follow
//   6  payload  the table's data: the ECU's reply less its 4-byte header
//               and its checksum (the adapter has checked that)
//
// A frame with len 0 is a heartbeat: the adapter is up, and flags says
// whether the ECU answers. No CRC: ESP-NOW frames carry the radio's FCS.
//
// The adapter broadcasts on kChannel; nothing is sent back to it. The dash's
// C6 passes every ESP-NOW frame that starts with kMagic to the P4 as an
// ESP-Hosted custom message, id kMsgId: the sender's 6-byte address, then
// the frame.
//
// Frames are lost (one radio shared with BLE), so nothing here needs every
// one. A Link is fed on one task; its Snapshot is what crosses to others.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::ecu {

inline constexpr uint8_t kMagic        = 0x4B;
inline constexpr uint8_t kVersion      = 1;
inline constexpr size_t  kHeaderLen    = 6;
inline constexpr uint8_t kFlagEcuAwake = 0x01;

// What both ends of the radio link agree on. The update hotspot uses the
// same channel, so the adapter is still heard while it is on.
inline constexpr uint8_t  kChannel = 6;
inline constexpr uint32_t kMsgId   = 0x454B4C31;   // "EKL1", C6 -> P4
inline constexpr size_t   kAddrLen = 6;

inline constexpr uint8_t kTableLive = 0x11;
inline constexpr size_t  kLiveLen   = 20;

// The adapter sends 10-20 frames a second; older than this is no data.
inline constexpr uint32_t kStaleMs = 500;

struct Frame {
    uint8_t seq   = 0;
    uint8_t flags = 0;
    uint8_t table = 0;
    uint8_t len   = 0;
    const uint8_t* payload = nullptr;   // into the buffer parsed, len bytes
};

// False unless `p` is exactly one frame of this version.
bool parse(const uint8_t* p, size_t n, Frame& out);

// Writes one frame, returns its length, 0 if it does not fit. For the
// adapter's firmware, the simulator and the tests.
size_t encode(uint8_t seq, uint8_t flags, uint8_t table,
              const uint8_t* payload, uint8_t len, uint8_t* out, size_t cap);

// Table 0x11. Units as the ECU gives them.
struct Reading {
    float    rpm       = 0.0f;
    float    tps_v     = 0.0f;
    float    tps_pct   = 0.0f;
    float    engine_c  = 0.0f;   // the ECU's engine temperature sensor
    float    intake_c  = 0.0f;
    float    map_kpa   = 0.0f;
    float    batt_v    = 0.0f;
    float    speed_kph = 0.0f;   // expected 0: the CRF250F has no speedometer
    uint16_t inj_raw   = 0;      // injector pulse, scale not known
};

// False for a payload shorter than kLiveLen.
bool decode_live(const uint8_t* payload, size_t n, Reading& out);

// Everything a Link knows, as plain data for a Published<> slot.
struct Snapshot {
    bool     heard = false;
    uint32_t heard_ms = 0;
    bool     ecu_awake = false;
    bool     have = false;
    uint32_t reading_ms = 0;
    Reading  reading{};
    // The adapter this power-on listens to: the first one heard.
    bool     latched = false;
    uint8_t  addr[kAddrLen] = {};
    // Since boot: frames missed by sequence number, frames that were not
    // ours, frames from some other adapter.
    uint32_t lost = 0;
    uint32_t rejected = 0;
    uint32_t foreign = 0;
};

// A frame arrived within kStaleMs.
bool adapter_online(const Snapshot& s, uint32_t now_ms);
// The latest live data, if the ECU is answering and it is fresh.
bool reading(const Snapshot& s, Reading& out, uint32_t now_ms);

class Link {
public:
    // One frame from `addr`. False for one that is not ours, from another
    // adapter, or a repeat of the last.
    // shortcut: the adapter is whoever is heard first each power-on, with no
    // encryption; store its address and add an ESP-NOW key if a second
    // adapter ever shares a paddock.
    bool feed(const uint8_t addr[kAddrLen], const uint8_t* p, size_t n, uint32_t now_ms);
    // The C6's custom message: address, then frame.
    bool feed_forwarded(const uint8_t* msg, size_t n, uint32_t now_ms);

    const Snapshot& snapshot() const { return s_; }

private:
    Snapshot s_{};
    uint8_t  seq_ = 0;
};

}  // namespace core::ecu
