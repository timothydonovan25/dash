#include "core/crash/crash.hpp"
#include <cmath>
#include <cstdio>

namespace core::crash {

State Detector::update(const Input& in, const Config& c) {
    const uint32_t now = in.t_ms;
    const float mag = std::sqrt(in.g_lat * in.g_lat + in.g_lon * in.g_lon + in.g_vert * in.g_vert);
    const bool tilted = mag > 0.5f &&
        in.g_vert / mag <= std::cos(c.down_tilt_deg * 3.14159265f / 180.0f);
    // Ridden upright, not merely ridden: a board mounted on its side (the
    // car tests, s298) is "down" all the time and must never arm.
    if (in.mph >= c.moving_mph && in.imu_ok && !tilted) {
        last_moving_ms_ = now;
        have_moving_ = true;
    }
    if (!in.enabled) {
        st_.phase = Phase::Idle;
        armed_ = down_ = false;
        return st_;
    }
    // Riding again ends it, whatever phase: whoever is on it is fine.
    if (st_.phase != Phase::Idle && in.mph >= c.moving_mph) {
        st_.phase = Phase::Idle;
        armed_ = down_ = false;
    }

    switch (st_.phase) {
        case Phase::Idle: {
            if (in.imu_ok && in.hit_ms != seen_hit_ms_ && in.hit_g >= c.impact_g) {
                seen_hit_ms_ = in.hit_ms;
                // Signed: the speed that proves riding may be newer than the hit.
                if (have_moving_ &&
                    static_cast<int32_t>(in.hit_ms - last_moving_ms_) <= static_cast<int32_t>(c.moving_ms)) {
                    armed_ = true;
                    armed_hit_ms_ = in.hit_ms;
                }
            }
            const bool down = in.imu_ok && tilted && in.mph < c.still_mph;
            if (!down) {
                down_ = false;
                if (armed_ && now - armed_hit_ms_ > c.after_hit_ms) armed_ = false;
                break;
            }
            if (!armed_) break;
            if (!down_) {
                down_ = true;
                down_since_ms_ = now;
            }
            if (now - down_since_ms_ >= c.down_ms) {
                st_.phase = Phase::Countdown;
                countdown_end_ms_ = now + c.countdown_ms;
                armed_ = down_ = false;
            }
            break;
        }
        case Phase::Countdown:
            if (static_cast<int32_t>(countdown_end_ms_ - now) <= 0) {
                st_.phase = Phase::Sent;
                if (++st_.sos_id == 0) st_.sos_id = 1;   // 0 is "none"
            }
            break;
        case Phase::Sent:
            break;
    }
    const int32_t left = static_cast<int32_t>(countdown_end_ms_ - now);
    st_.countdown_s = st_.phase == Phase::Countdown && left > 0
        ? static_cast<uint8_t>((left + 999) / 1000) : 0;
    return st_;
}

void Detector::cancel() {
    if (st_.phase == Phase::Sent) st_.ok_id = st_.sos_id;
    st_.phase = Phase::Idle;
    st_.countdown_s = 0;
    armed_ = down_ = false;
}

void Detector::test(uint32_t now_ms, const Config& c) {
    st_.phase = Phase::Countdown;
    countdown_end_ms_ = now_ms + c.countdown_ms;
    armed_ = down_ = false;
}

size_t sos_line(uint16_t id, bool have_fix, double lat, double lon, int32_t age_s,
                char* out, size_t n) {
    const int k = have_fix
        ? std::snprintf(out, n, "X %u %.6f %.6f %ld", static_cast<unsigned>(id), lat, lon,
                        static_cast<long>(age_s))
        : std::snprintf(out, n, "X %u - - -1", static_cast<unsigned>(id));
    return k > 0 && static_cast<size_t>(k) < n ? static_cast<size_t>(k) : 0;
}

size_t ok_line(uint16_t id, char* out, size_t n) {
    const int k = std::snprintf(out, n, "X %u ok", static_cast<unsigned>(id));
    return k > 0 && static_cast<size_t>(k) < n ? static_cast<size_t>(k) : 0;
}

bool parse_ack(const uint8_t* data, size_t len, uint16_t& id, Ack& ack) {
    if (len < 5 || data[0] != 'K' || data[1] != ' ') return false;
    size_t i = 2;
    uint32_t v = 0;
    const size_t digits_at = i;
    while (i < len && data[i] >= '0' && data[i] <= '9' && v <= 0xFFFF) v = v * 10 + (data[i++] - '0');
    if (i == digits_at || v == 0 || v > 0xFFFF || i + 2 > len || data[i] != ' ') return false;
    const uint8_t code = data[i + 1];
    if (code != '0' && code != '1' && code != '2') return false;
    id = static_cast<uint16_t>(v);
    ack = static_cast<Ack>(code);
    return true;
}

}  // namespace core::crash
