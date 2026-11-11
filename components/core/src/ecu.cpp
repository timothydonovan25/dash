#include "core/ecu/ecu.hpp"

#include <cstring>

namespace core::ecu {

bool parse(const uint8_t* p, size_t n, Frame& out) {
    if (!p || n < kHeaderLen) return false;
    if (p[0] != kMagic || p[1] != kVersion) return false;
    if (n != kHeaderLen + p[5]) return false;
    out.seq = p[2];
    out.flags = p[3];
    out.table = p[4];
    out.len = p[5];
    out.payload = p + kHeaderLen;
    return true;
}

size_t encode(uint8_t seq, uint8_t flags, uint8_t table,
              const uint8_t* payload, uint8_t len, uint8_t* out, size_t cap) {
    const size_t n = kHeaderLen + len;
    if (!out || cap < n || (len && !payload)) return 0;
    out[0] = kMagic;
    out[1] = kVersion;
    out[2] = seq;
    out[3] = flags;
    out[4] = table;
    out[5] = len;
    if (len) std::memcpy(out + kHeaderLen, payload, len);
    return n;
}

bool decode_live(const uint8_t* b, size_t n, Reading& out) {
    if (!b || n < kLiveLen) return false;
    const auto volts = [](uint8_t v) { return v * 5.0f / 256.0f; };
    out.rpm = static_cast<float>((b[0] << 8) | b[1]);
    out.tps_v = volts(b[2]);
    // The sources disagree (x0.5 or /16). x0.5 is the one that gives a
    // believable opening in the CBR600RR sample at 10800 rpm; check it on
    // the bike against a closed and a wide-open throttle.
    const float tps = b[3] * 0.5f;
    out.tps_pct = tps > 100.0f ? 100.0f : tps;
    out.engine_c = static_cast<float>(b[5]) - 40.0f;
    out.intake_c = static_cast<float>(b[7]) - 40.0f;
    out.map_kpa = static_cast<float>(b[9]);
    out.batt_v = b[12] / 10.0f;
    out.speed_kph = static_cast<float>(b[13]);
    out.inj_raw = static_cast<uint16_t>((b[14] << 8) | b[15]);
    return true;
}

bool adapter_online(const Snapshot& s, uint32_t now_ms) {
    return s.heard && now_ms - s.heard_ms <= kStaleMs;
}

bool reading(const Snapshot& s, Reading& out, uint32_t now_ms) {
    if (!s.have || !s.ecu_awake || now_ms - s.reading_ms > kStaleMs) return false;
    out = s.reading;
    return true;
}

bool Link::feed(const uint8_t addr[kAddrLen], const uint8_t* p, size_t n, uint32_t now_ms) {
    Frame f;
    if (!addr || !parse(p, n, f)) {
        ++s_.rejected;
        return false;
    }
    if (!s_.latched) {
        std::memcpy(s_.addr, addr, kAddrLen);
        s_.latched = true;
    } else if (std::memcmp(s_.addr, addr, kAddrLen) != 0) {
        ++s_.foreign;
        return false;
    }
    // After a silence the adapter may have slept or restarted, so its
    // sequence says nothing about what was missed.
    if (adapter_online(s_, now_ms)) {
        if (f.seq == seq_) return false;   // the radio's retry of the last
        s_.lost += static_cast<uint8_t>(f.seq - seq_ - 1);
    }
    s_.heard = true;
    seq_ = f.seq;
    s_.heard_ms = now_ms;
    s_.ecu_awake = (f.flags & kFlagEcuAwake) != 0;
    if (f.table == kTableLive && decode_live(f.payload, f.len, s_.reading)) {
        s_.have = true;
        s_.reading_ms = now_ms;
    }
    return true;
}

bool Link::feed_forwarded(const uint8_t* msg, size_t n, uint32_t now_ms) {
    if (!msg || n < kAddrLen) {
        ++s_.rejected;
        return false;
    }
    return feed(msg, msg + kAddrLen, n - kAddrLen, now_ms);
}

}  // namespace core::ecu
