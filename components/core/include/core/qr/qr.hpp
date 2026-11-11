// A QR code for the Wi-Fi join string (DIAG -> Update, §11.7.1).
//
// Byte mode, version 3 (29x29), error correction M: up to 42 bytes, which
// "WIFI:T:WPA;S:CRF250F-Dash;P:<8 digits>;;" (38) fits with room. A phone's
// camera app reads it and offers to join.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::qr {

inline constexpr int kSize = 29;
inline constexpr size_t kMaxBytes = 42;

struct Code {
    // Bit 0: dark. Bit 1: a function module (finder, timing, format), used
    // while building.
    uint8_t m[kSize][kSize] = {};
    bool dark(int x, int y) const { return m[y][x] & 1; }
};

// False, and `out` untouched, for text over kMaxBytes.
bool encode(const char* text, Code& out);

// "WIFI:T:WPA;S:<ssid>;P:<pass>;;", the form phone cameras join from.
// shortcut: no escaping of ; , : \ " -- the SSID is fixed and the password
// digits; escape them here if either ever changes.
size_t wifi_join(char* out, size_t n, const char* ssid, const char* pass);

}  // namespace core::qr
