#include "core/hr/hr.hpp"

#include <cstring>

namespace core::hr {

bool parse(const uint8_t* p, size_t n, Measurement& out) {
    out = Measurement{};
    if (!p || n < 2) return false;
    const uint8_t flags = p[0];
    size_t k = 1;
    if (flags & 0x01) {               // bpm is uint16
        if (n < k + 2) return false;
        out.bpm = static_cast<uint16_t>(p[k] | (p[k + 1] << 8));
        k += 2;
    } else {
        out.bpm = p[k];
        k += 1;
    }
    // Bits 1-2: 0b10 supported, no contact; 0b11 supported, contact.
    out.contact_known = (flags & 0x04) != 0;
    out.contact = out.contact_known && (flags & 0x02) != 0;
    if (flags & 0x08) {               // energy expended, uint16, unused
        if (n < k + 2) return false;
        k += 2;
    }
    if (flags & 0x10) {               // RR intervals, uint16 each, 1/1024 s
        while (k + 1 < n) {
            const uint16_t rr = static_cast<uint16_t>(p[k] | (p[k + 1] << 8));
            if (out.rr_count < 4) {
                out.rr_ms[out.rr_count++] = static_cast<uint16_t>((rr * 1000u + 512u) / 1024u);
            }
            k += 2;
        }
    }
    return out.bpm != 0;
}

Advert parse_advert(const uint8_t* ad, size_t n) {
    Advert a;
    size_t k = 0;
    while (ad && k < n) {
        const uint8_t len = ad[k];
        if (len == 0 || k + 1 + len > n) break;
        const uint8_t type = ad[k + 1];
        const uint8_t* v = ad + k + 2;
        const size_t vn = len - 1u;
        if (type == 0x02 || type == 0x03) {        // 16-bit service UUIDs
            for (size_t i = 0; i + 1 < vn; i += 2) {
                if ((v[i] | (v[i + 1] << 8)) == kServiceUuid) a.hrs = true;
            }
        } else if (type == 0x08 || type == 0x09) { // short or complete name
            // A complete name wins over a short one.
            if (type == 0x09 || a.name[0] == '\0') {
                const size_t m = vn < sizeof a.name - 1 ? vn : sizeof a.name - 1;
                for (size_t i = 0; i < m; ++i) {
                    const char c = static_cast<char>(v[i]);
                    a.name[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
                }
                a.name[m] = '\0';
            }
        }
        k += 1u + len;
    }
    return a;
}

bool same(const Addr& a, const Addr& b) {
    return a.type == b.type && std::memcmp(a.val, b.val, sizeof a.val) == 0;
}

void Picker::begin_scan() {
    have_ = false;
    best_rssi_ = -128;
    best_name_[0] = '\0';
}

bool Picker::matches_latch(const Addr& a, const char* name) const {
    if (same(a, latch_)) return true;
    return name_[0] != '\0' && name && std::strcmp(name, name_) == 0;
}

void Picker::heard(const Addr& a, int8_t rssi_dbm, const char* name) {
    if (latched_) {
        // Only the rider's own; the floor does not apply to a known sensor.
        if (!matches_latch(a, name)) return;
    } else if (rssi_dbm < kRssiFloorDbm) {
        return;
    }
    // The same device heard again with a name it lacked before keeps it.
    if (have_ && same(a, best_)) {
        if (rssi_dbm > best_rssi_) best_rssi_ = rssi_dbm;
        if (name && name[0] && best_name_[0] == '\0') {
            std::strncpy(best_name_, name, sizeof best_name_ - 1);
            best_name_[sizeof best_name_ - 1] = '\0';
        }
        return;
    }
    if (!have_ || rssi_dbm > best_rssi_) {
        have_ = true;
        best_ = a;
        best_rssi_ = rssi_dbm;
        best_name_[0] = '\0';
        if (name) {
            std::strncpy(best_name_, name, sizeof best_name_ - 1);
            best_name_[sizeof best_name_ - 1] = '\0';
        }
    }
}

bool Picker::choice(Addr& out) const {
    if (!have_) return false;
    out = best_;
    return true;
}

void Picker::latch(const Addr& a, const char* name) {
    latched_ = true;
    latch_ = a;
    // Keep the name the latch was made with if this connect brought none.
    if (name && name[0]) {
        std::strncpy(name_, name, sizeof name_ - 1);
        name_[sizeof name_ - 1] = '\0';
    } else if (same(a, best_) && best_name_[0]) {
        std::strncpy(name_, best_name_, sizeof name_ - 1);
        name_[sizeof name_ - 1] = '\0';
    }
}

void Picker::reset() {
    latched_ = false;
    latch_ = Addr{};
    name_[0] = '\0';
    begin_scan();
}

void Session::add(uint16_t bpm) {
    if (bpm == 0) return;
    ++count_;
    sum_ += bpm;
    if (bpm > max_) max_ = bpm;
    if (min_ == 0 || bpm < min_) min_ = bpm;
}

uint16_t Session::avg_bpm() const {
    if (count_ == 0) return 0;
    return static_cast<uint16_t>((sum_ + count_ / 2) / count_);
}

bool fresh(const state::Heart& h, uint32_t now_ms) {
    // Signed: a frame's clock is read before its slots, so a reading that
    // lands in between is a few ms *ahead* of `now_ms`. Unsigned, that was
    // four billion ms old, and STAT -> HEART blinked to "--" for a frame on
    // every reading.
    const int32_t age = static_cast<int32_t>(now_ms - h.at_ms);
    return h.link == state::Heart::Live && h.bpm != 0 && h.at_ms != 0 &&
           age < static_cast<int32_t>(kStaleMs);
}

uint8_t zone(uint16_t bpm, uint16_t max_bpm) {
    if (max_bpm == 0 || bpm == 0) return 0;
    const uint32_t pct = bpm * 100u / max_bpm;
    if (pct >= 90) return 5;
    if (pct >= 80) return 4;
    if (pct >= 70) return 3;
    if (pct >= 60) return 2;
    if (pct >= 50) return 1;
    return 0;
}

}  // namespace core::hr
