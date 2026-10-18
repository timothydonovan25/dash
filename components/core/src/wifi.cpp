#include "core/wifi/wifi.hpp"

namespace core::wifi {

void make_password(uint32_t r0, uint32_t r1, char* out) {
    // Four digits from each word. The modulo bias over 2^32 is under one
    // part in four hundred thousand -- nothing an attacker can use against
    // a password that lasts ten minutes.
    uint32_t a = r0 % 10000u;
    uint32_t b = r1 % 10000u;
    for (int i = 3; i >= 0; --i) { out[i] = static_cast<char>('0' + a % 10u); a /= 10u; }
    for (int i = 7; i >= 4; --i) { out[i] = static_cast<char>('0' + b % 10u); b /= 10u; }
    out[kPassLen] = '\0';
}

bool should_stop(uint32_t now_ms, uint32_t last_use_ms, float speed_mph) {
    if (speed_mph > kMovingMph) return true;
    // Unsigned, so a wrapped counter reads as a short gap, not 49 days.
    return now_ms - last_use_ms >= kIdleOffMs;
}

uint32_t off_in_s(uint32_t now_ms, uint32_t last_use_ms) {
    const uint32_t used = now_ms - last_use_ms;
    if (used >= kIdleOffMs) return 0;
    return (kIdleOffMs - used + 999u) / 1000u;
}

namespace {
// Letters, digits, '-', '_' and exactly one '.', under 32 characters, ending
// in `ext` (".csv"), compared without case when `any_case`.
bool valid_name(const char* name, const char* ext4, bool any_case) {
    if (!name || !*name) return false;
    size_t n = 0;
    int dots = 0;
    for (const char* p = name; *p; ++p, ++n) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
        if (c == '.') ++dots;
        if (n >= 31) return false;            // FAT long names, kept sane
    }
    if (dots != 1 || n < 5) return false;
    const char* ext = name + n - 4;
    for (int i = 0; i < 4; ++i) {
        char c = ext[i];
        if (any_case && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != ext4[i]) return false;
    }
    return true;
}
}  // namespace

bool valid_log_name(const char* name) { return valid_name(name, ".csv", false); }

bool valid_game_name(const char* name) { return valid_name(name, ".wad", true); }

const char* state_name(State s) {
    switch (s) {
        case State::Off:        return "off";
        case State::Starting:   return "starting";
        case State::On:         return "on";
        case State::Failed:     return "failed";
        case State::UpdatingC6: return "updating C6";
    }
    return "?";
}

}  // namespace core::wifi
