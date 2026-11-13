#include "core/tpms/tpms.hpp"

namespace core::tpms {
namespace {

int32_t le32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                static_cast<uint32_t>(p[1]) << 8 |
                                static_cast<uint32_t>(p[2]) << 16 |
                                static_cast<uint32_t>(p[3]) << 24);
}

constexpr float kPaPerPsi = 6894.757f;

}  // namespace

bool decode_0100(const uint8_t* mfg, size_t n, Reading& out) {
    if (!mfg || n != 18 || mfg[0] != 0x00 || mfg[1] != 0x01) return false;
    if ((mfg[2] & 0xF0) != 0x80) return false;   // 0x80 + the sensor's number
    const int32_t pa = le32(mfg + 8);
    const int32_t centi_c = le32(mfg + 12);
    // Past these it is someone else's data that happens to share the ID.
    if (pa < 0 || pa > 1000000 || centi_c < -4000 || centi_c > 12500 || mfg[16] > 100) {
        return false;
    }
    const uint32_t id = (static_cast<uint32_t>(mfg[5]) << 16 |
                         static_cast<uint32_t>(mfg[6]) << 8 | mfg[7]) & kIdMask;
    if (id == 0) return false;
    out.id = id;
    out.psi = static_cast<float>(pa) / kPaPerPsi;
    out.temp_c = static_cast<float>(centi_c) / 100.0f;
    out.batt_pct = mfg[16];
    out.alarm = mfg[17] != 0;
    return true;
}

const uint8_t* find_mfg(const uint8_t* ad, size_t n, size_t& mfg_len) {
    size_t k = 0;
    while (ad && k < n) {
        const uint8_t len = ad[k];
        if (len == 0 || k + 1 + len > n) break;
        if (ad[k + 1] == 0xFF) {
            mfg_len = len - 1u;
            return ad + k + 2;
        }
        k += 1u + len;
    }
    mfg_len = 0;
    return nullptr;
}

bool decode(const uint8_t* ad, size_t n, Reading& out) {
    size_t m = 0;
    const uint8_t* mfg = find_mfg(ad, n, m);
    if (!mfg) return false;
    return decode_0100(mfg, m, out);
}

void Tracker::learn(Wheel w, uint32_t now_ms) {
    learning_ = true;
    learn_wheel_ = w;
    learn_until_ = now_ms + kLearnMs;
}

void Tracker::bound(const uint32_t ids[2], uint32_t out[2]) {
    for (int w = 0; w < 2; ++w) {
        const uint32_t id = ids[w] & kIdMask;
        if (pending_[w] && id != ids_then_[w]) pending_[w] = 0;   // it landed, or was overridden
        out[w] = pending_[w] ? pending_[w] : id;
    }
}

Tracker::Learned Tracker::on_reading(const Reading& r, const uint32_t ids[2],
                                     uint32_t now_ms) {
    Learned l;
    uint32_t bound[2];
    this->bound(ids, bound);
    if (learning_) {
        const int w = static_cast<int>(learn_wheel_);
        if (r.id != bound[1 - w]) {
            learning_ = false;
            l.any = true;
            l.wheel = learn_wheel_;
            l.id = r.id;
            // The ConfigSet lands later; this wheel is the sensor's from now.
            pending_[w] = r.id;
            ids_then_[w] = ids[w] & kIdMask;
            bound[w] = r.id;
        }
    }
    for (int w = 0; w < 2; ++w) {
        if (bound[w] == 0 || r.id != bound[w]) continue;
        state::TpmsWheel& t = w == 0 ? out_.front : out_.rear;
        t.psi = r.psi;
        t.temp_c = r.temp_c;
        t.batt_pct = r.batt_pct;
        t.alarm = r.alarm;
        t.age_ms = 0;
        t.valid = true;
        last_ms_[w] = now_ms;
        last_id_[w] = r.id;
    }
    return l;
}

bool Tracker::tick(const uint32_t ids[2], uint32_t now_ms) {
    bool changed = false;
    uint32_t bound[2];
    this->bound(ids, bound);
    if (learning_ && static_cast<int32_t>(now_ms - learn_until_) >= 0) {
        learning_ = false;
        changed = true;
    }
    for (int w = 0; w < 2; ++w) {
        state::TpmsWheel& t = w == 0 ? out_.front : out_.rear;
        if (!t.valid) continue;
        t.age_ms = now_ms - last_ms_[w];
        // Stale, or the wheel was re-learned (or cleared) to another sensor.
        if (t.age_ms >= kStaleMs || last_id_[w] != bound[w]) {
            t = state::TpmsWheel{};
            changed = true;
        }
    }
    return changed;
}

}  // namespace core::tpms
