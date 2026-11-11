#include "core/media/feed.hpp"

#include <cstring>

namespace core::media {
namespace {

// §11.1's `D` and `P` are decimal ASCII milliseconds. Stops at the first
// non-digit and saturates rather than wrapping, so a garbage value draws a
// wrong number and never a negative-looking one.
uint32_t parse_ms(const uint8_t* s, size_t len) {
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + static_cast<uint64_t>(s[i] - '0');
        if (v > UINT32_MAX) return UINT32_MAX;
    }
    return static_cast<uint32_t>(v);
}

// Elapsed play time folded into the anchor. Guarded against a clock that
// appears to run backwards, which an anchor set before a reboot would.
void fold_elapsed(state::Media& m, uint32_t now_ms) {
    if (now_ms >= m.position_at_ms) m.position_ms += now_ms - m.position_at_ms;
    m.position_at_ms = now_ms;
}

}  // namespace

size_t copy_field(char* dst, size_t cap, const uint8_t* src, size_t len) {
    if (cap == 0) return 0;
    size_t n = len < cap - 1 ? len : cap - 1;
    // Cut short: walk back off any continuation bytes, then off the lead
    // byte that started them, unless the whole sequence fits.
    if (n < len) {
        size_t start = n;
        while (start > 0 && (src[start] & 0xC0) == 0x80) --start;
        n = start;
    }
    // A NUL from the phone ends the string here rather than leaving a field
    // whose strlen disagrees with what was written.
    const void* nul = std::memchr(src, 0, n);
    if (nul) n = static_cast<size_t>(static_cast<const uint8_t*>(nul) - src);
    std::memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

char apply_line(state::Media& m, const uint8_t* data, size_t len,
                uint32_t now_ms) {
    // The end of the write is the end of the line; a newline is optional.
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) --len;
    if (len == 0) return 0;

    const char tag = static_cast<char>(data[0]);
    const uint8_t* v = data + 1;
    size_t vlen = len - 1;
    // "T Foo" and "TFoo" read the same: one separating space is optional.
    if (vlen > 0 && v[0] == ' ') { ++v; --vlen; }

    switch (tag) {
        case 'T': {
            char buf[sizeof(m.title)];
            copy_field(buf, sizeof(buf), v, vlen);
            // A new track restarts the clock and clears the lyric here,
            // rather than trusting a `P`, `D` and `Y` to follow: they are
            // separate writes with no ordering guarantee, and a stale anchor
            // surviving a skip is a progress ring that climbs straight
            // through the change of song.
            if (std::strcmp(buf, m.title) != 0) {
                m.position_ms = 0;
                m.duration_ms = 0;
                m.position_at_ms = now_ms;
                m.lyric[0] = '\0';
            }
            std::memcpy(m.title, buf, sizeof(buf));
            break;
        }
        case 'A': copy_field(m.artist, sizeof(m.artist), v, vlen); break;
        case 'L': copy_field(m.album,  sizeof(m.album),  v, vlen); break;
        case 'Y': copy_field(m.lyric,  sizeof(m.lyric),  v, vlen); break;
        case 'S': {
            if (vlen == 0 || v[0] < '0' || v[0] > '2') return 0;
            const uint8_t s = static_cast<uint8_t>(v[0] - '0');
            // Leaving play freezes the extrapolated position where it is;
            // entering play restarts the anchor so paused time is not
            // counted as played.
            if (m.state == 1 && s != 1) fold_elapsed(m, now_ms);
            if (s == 1 && m.state != 1) m.position_at_ms = now_ms;
            m.state = s;
            break;
        }
        case 'D': m.duration_ms = parse_ms(v, vlen); break;
        case 'P':
            m.position_ms = parse_ms(v, vlen);
            m.position_at_ms = now_ms;
            break;
        default:
            return 0;
    }
    m.src = kSrcPhone;
    m.updated_ms = now_ms != 0 ? now_ms : 1;   // 0 means "no media at all"
    return tag;
}

namespace {
// Drawn text must not hold control bytes: the font has no glyph for them.
void blank_controls(char* s) {
    for (; *s; ++s) {
        if (static_cast<uint8_t>(*s) < 0x20 || *s == 0x7F) *s = ' ';
    }
}
}  // namespace

bool apply_notify(state::Notify& n, const uint8_t* data, size_t len, uint32_t now_ms) {
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) --len;
    if (len < 2 || data[0] != 'N') return false;
    const char kind = static_cast<char>(data[1]);
    if (kind != 'C' && kind != 'E' && kind != 'X' && kind != 'M' && kind != 'O') return false;
    if (kind == 'E') {
        n.ringing = false;
        return true;
    }
    const uint8_t* v = data + 2;
    size_t vlen = len - 2;
    if (vlen > 0 && v[0] == ' ') { ++v; --vlen; }
    const void* us = std::memchr(v, 0x1F, vlen);
    const size_t who_len = us ? static_cast<size_t>(static_cast<const uint8_t*>(us) - v) : vlen;
    copy_field(n.who, sizeof n.who, v, who_len);
    if (us) {
        copy_field(n.text, sizeof n.text, v + who_len + 1, vlen - who_len - 1);
    } else {
        n.text[0] = '\0';
    }
    blank_controls(n.who);
    blank_controls(n.text);
    n.kind = kind;
    n.ringing = kind == 'C';
    n.at_ms = now_ms != 0 ? now_ms : 1;
    ++n.seq;
    if (n.seq == 0) n.seq = 1;
    return true;
}

bool ringing(const state::Notify& n, uint32_t now_ms) {
    // Signed: the call can land after the frame's clock was read, a few ms
    // ahead of `now_ms`; unsigned, the toast dropped for that frame.
    return n.ringing && static_cast<int32_t>(now_ms - n.at_ms) < static_cast<int32_t>(kRingTimeoutMs);
}

bool parse_button(const uint8_t* data, size_t len, nav::InputEvent& e) {
    // A trailing newline is allowed, as on every Feed line.
    if (len == 4 && data[3] == '\n') len = 3;
    if (len != 3 || data[0] != 'B' || data[1] != ' ') return false;
    const uint8_t c = data[2];
    if (c >= 'A' && c <= 'E') e = {static_cast<nav::Button>(c - 'A'), nav::Press::Short};
    else if (c >= 'a' && c <= 'e') e = {static_cast<nav::Button>(c - 'a'), nav::Press::Long};
    else return false;
    return true;
}

size_t ctrl_line(uint8_t cmd, uint8_t out[3]) {
    if (cmd > 2) return 0;
    out[0] = 'C';
    out[1] = ' ';
    out[2] = static_cast<uint8_t>('0' + cmd);
    return 3;
}

}  // namespace core::media
