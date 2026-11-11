// QR encoder, version 3-M only (ISO/IEC 18004). The steps follow Nayuki's
// QR Code generator: function patterns, data + Reed-Solomon, the zigzag
// placement, a mask, the format bits.
#include "core/qr/qr.hpp"

#include <cstdio>
#include <cstring>

namespace core::qr {
namespace {

constexpr int kDataWords = 44;   // version 3-M: 70 codewords, 26 of them ECC
constexpr int kEccWords = 26;
constexpr uint8_t kDark = 1, kFunc = 2;

// Mask 0: (x + y) even. Any mask decodes; this one is fixed so the code
// for a given text never changes.
// shortcut: no penalty scoring across the eight masks; add it if a phone
// ever struggles with one password.
constexpr int kMask = 0;

void set(Code& c, int x, int y, bool dark) {
    if (x < 0 || y < 0 || x >= kSize || y >= kSize) return;
    c.m[y][x] = static_cast<uint8_t>(kFunc | (dark ? kDark : 0));
}

void finder(Code& c, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            const int d = dx * dx > dy * dy ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            set(c, cx + dx, cy + dy, d != 2 && d != 4);
        }
}

void alignment(Code& c, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const int d = dx * dx > dy * dy ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            set(c, cx + dx, cy + dy, d != 1);
        }
}

void format_bits(Code& c) {
    const int data = (0 << 3) | kMask;   // M is 00
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;
    auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) set(c, 8, i, bit(i));
    set(c, 8, 7, bit(6));
    set(c, 8, 8, bit(7));
    set(c, 7, 8, bit(8));
    for (int i = 9; i < 15; ++i) set(c, 14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) set(c, kSize - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) set(c, 8, kSize - 15 + i, bit(i));
    set(c, 8, kSize - 8, true);   // the dark module
}

uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

void reed_solomon(const uint8_t* data, uint8_t* ecc) {
    uint8_t div[kEccWords] = {};
    div[kEccWords - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < kEccWords; ++i) {
        for (int j = 0; j < kEccWords; ++j) {
            div[j] = gf_mul(div[j], root);
            if (j + 1 < kEccWords) div[j] ^= div[j + 1];
        }
        root = gf_mul(root, 2);
    }
    std::memset(ecc, 0, kEccWords);
    for (int k = 0; k < kDataWords; ++k) {
        const uint8_t f = data[k] ^ ecc[0];
        std::memmove(ecc, ecc + 1, kEccWords - 1);
        ecc[kEccWords - 1] = 0;
        for (int i = 0; i < kEccWords; ++i) ecc[i] ^= gf_mul(div[i], f);
    }
}

}  // namespace

bool encode(const char* text, Code& out) {
    const size_t len = std::strlen(text);
    if (len > kMaxBytes) return false;

    // Mode 0100, an 8-bit count, the bytes, up to four 0s, then pad bytes.
    uint8_t words[kDataWords + kEccWords] = {};
    int bit = 0;
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i, ++bit)
            if ((v >> i) & 1) words[bit >> 3] |= static_cast<uint8_t>(0x80 >> (bit & 7));
    };
    put(0x4, 4);
    put(static_cast<uint32_t>(len), 8);
    for (size_t i = 0; i < len; ++i) put(static_cast<uint8_t>(text[i]), 8);
    bit += 4;                                  // the terminator (already 0)
    for (int k = (bit + 7) / 8, p = 0; k < kDataWords; ++k, p ^= 1)
        words[k] = p ? 0x11 : 0xEC;
    reed_solomon(words, words + kDataWords);

    Code c;
    for (int i = 0; i < kSize; ++i) {           // timing
        set(c, 6, i, i % 2 == 0);
        set(c, i, 6, i % 2 == 0);
    }
    finder(c, 3, 3);
    finder(c, kSize - 4, 3);
    finder(c, 3, kSize - 4);
    alignment(c, 22, 22);
    format_bits(c);

    // Two columns at a time from the right, up then down, skipping the
    // vertical timing column. Version 3's 7 remainder bits stay light.
    int i = 0;
    for (int right = kSize - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < kSize; ++vert)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const int y = ((right + 1) & 2) == 0 ? kSize - 1 - vert : vert;
                if (c.m[y][x] & kFunc) continue;
                if (i < (kDataWords + kEccWords) * 8) {
                    if ((words[i >> 3] >> (7 - (i & 7))) & 1) c.m[y][x] |= kDark;
                    ++i;
                }
                if ((x + y) % 2 == 0) c.m[y][x] ^= kDark;   // mask 0
            }
    }
    out = c;
    return true;
}

size_t wifi_join(char* out, size_t n, const char* ssid, const char* pass) {
    const int k = std::snprintf(out, n, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    return k < 0 ? 0 : static_cast<size_t>(k);
}

}  // namespace core::qr
