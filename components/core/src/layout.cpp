// The deck: the stock layout, validation, and the .dui format.
#include "core/layout/deck.hpp"
#include "core/config/config.hpp"
#include "core/layout/fields.hpp"
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace core::layout {

namespace {
constexpr const char* kBuiltinNames[] = {
    nullptr,
    "SPEED", "MEDIA", "TRIP", "MAP", "NAV", "MINIMAL", "PHONE",
    "RACE", "LAP", "LAPS", "TRACK MAP", "SESSION",
    "ALL", "ENGINE", "IMU", "PERF", "STUNT", "HEART",
    "DIAG", "SENSORS", "GPS", "FUSION", "SYSTEM", "POWER", "BUS", "RADIO", "UPDATE",
    "CONFIG", "GEAR CAL",
};
static_assert(sizeof kBuiltinNames / sizeof kBuiltinNames[0] == kBuiltinEnd,
              "a name for every built-in screen");

void set_name(char* dst, const char* s) {
    std::strncpy(dst, s, kNameLen - 1);
    dst[kNameLen - 1] = '\0';
}

void add_mode(Deck& d, const char* name, std::initializer_list<uint8_t> ids) {
    ModeDef& m = d.modes[d.mode_count++];
    set_name(m.name, name);
    for (uint8_t id : ids) m.screens[m.count++] = id;
}

bool fail(Error* why, const char* what, int index = -1) {
    if (why) { why->what = what; why->index = index; }
    return false;
}

// A NUL inside the array, and only printable ASCII before it.
bool good_text(const char* s, size_t n, bool allow_empty) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) {
        if (s[i] < 0x20 || s[i] > 0x7e) return false;
    }
    return i < n && (allow_empty || i > 0);
}

struct Box { int16_t x0, y0, x1, y1; };   // x1, y1 exclusive

bool overlaps(const Box& a, const Box& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
bool contains(const Box& outer, const Box& in) {
    return outer.x0 <= in.x0 && outer.y0 <= in.y0 && outer.x1 >= in.x1 && outer.y1 >= in.y1;
}

bool default_field(uint16_t id) { return find_field(id) != nullptr; }
bool default_action(uint16_t key) {
    const config::ConfigItem* it = config::find(key);
    // A key this firmware does not have (retired, or from a newer one) is
    // let through and does nothing; a key that is a setting, not an action,
    // is a mistake.
    return !it || it->kind == config::Kind::Action;
}
}  // namespace

const char* builtin_name(uint8_t id) {
    return id < kBuiltinEnd ? kBuiltinNames[id] : nullptr;
}

void default_deck(Deck& d) {
    d = Deck{};
    add_mode(d, "CRUISE", {kSpeed, kMedia, kTrip, kTripMap, kNav, kMinimal, kPhone});
    add_mode(d, "TRACK",  {kRace, kLap, kLapHistory, kTrackMap, kSession});
    add_mode(d, "STAT",   {kAllData, kEngine, kAttitude, kPerf, kStunt, kHeart});
}

void chrome_box(const Chrome& c, int16_t& x0, int16_t& y0, int16_t& x1, int16_t& y1) {
    auto lo = [](int a, int b) { return static_cast<int16_t>(a < b ? a : b); };
    auto hi = [](int a, int b) { return static_cast<int16_t>(a > b ? a : b); };
    switch (c.shape) {
        case Shape::Line:
            x0 = lo(c.x, c.x + c.w); x1 = static_cast<int16_t>(hi(c.x, c.x + c.w) + 1);
            y0 = lo(c.y, c.y + c.h); y1 = static_cast<int16_t>(hi(c.y, c.y + c.h) + 1);
            return;
        case Shape::Triangle:
            x0 = lo(lo(c.x, c.x + c.w), c.x3); x1 = static_cast<int16_t>(hi(hi(c.x, c.x + c.w), c.x3) + 1);
            y0 = lo(lo(c.y, c.y + c.h), c.y3); y1 = static_cast<int16_t>(hi(hi(c.y, c.y + c.h), c.y3) + 1);
            return;
        case Shape::Text: {
            const int n = static_cast<int>(strnlen(c.text, kTextLen));
            const int w = n ? (n * 6 - 1) * c.scale : 0;
            int left = c.x;
            if (c.align == 1) left = c.x - w / 2;
            else if (c.align == 2) left = c.x - w;
            x0 = static_cast<int16_t>(left); x1 = static_cast<int16_t>(left + w);
            y0 = c.y; y1 = static_cast<int16_t>(c.y + 7 * c.scale);
            return;
        }
        default:
            x0 = c.x; y0 = c.y;
            x1 = static_cast<int16_t>(c.x + c.w); y1 = static_cast<int16_t>(c.y + c.h);
            return;
    }
}

bool validate(const Deck& d, Error* why, const Known& known) {
    const Known k{known.field ? known.field : default_field,
                  known.action ? known.action : default_action};

    if (d.mode_count == 0) return fail(why, "no riding mode");
    if (d.mode_count > kMaxModes) return fail(why, "too many modes");
    if (d.screen_count > kMaxCustom) return fail(why, "too many screens");
    if (d.chrome_count > kMaxChrome) return fail(why, "too many shapes");
    if (d.readout_count > kMaxReadouts) return fail(why, "too many readouts");
    if (d.image_count > kMaxImages) return fail(why, "too many images");
    if (d.image_bytes > kImagePool) return fail(why, "images too big");

    for (int i = 0; i < d.mode_count; ++i) {
        const ModeDef& m = d.modes[i];
        if (!good_text(m.name, kNameLen, false)) return fail(why, "mode name", i);
        if (m.count == 0) return fail(why, "mode with no screens", i);
        if (m.count > kMaxScreensPerMode) return fail(why, "too many screens in a mode", i);
        for (int j = 0; j < m.count; ++j) {
            const uint8_t id = m.screens[j];
            const bool riding = id >= kFirstRiding && id <= kLastRiding;
            const bool custom = id >= kCustomBase && id - kCustomBase < d.screen_count;
            if (!riding && !custom) return fail(why, "screen not allowed in a mode", i);
        }
    }

    for (int i = 0; i < d.image_count; ++i) {
        const Image& im = d.images[i];
        if (im.w == 0 || im.h == 0 || im.w > kMaxImageSide || im.h > kMaxImageSide)
            return fail(why, "image size", i);
        if (im.offset + ((im.w + 7) / 8) * im.h > d.image_bytes) return fail(why, "image bits", i);
    }

    for (int si = 0; si < d.screen_count; ++si) {
        const Screen& s = d.screens[si];
        if (!good_text(s.name, kNameLen, false)) return fail(why, "screen name", si);
        if (s.chrome_first + s.chrome_n > d.chrome_count) return fail(why, "shape range", si);
        if (s.readout_first + s.readout_n > d.readout_count) return fail(why, "readout range", si);
        if (s.readout_n > kMaxReadoutsPerScreen) return fail(why, "too many readouts on a screen", si);
        if (s.e_action && !k.action(s.e_action)) return fail(why, "E is not an action", si);
        const int16_t top = (s.flags & Screen::kTitle) ? kTitleH : 0;

        for (int c = s.chrome_first; c < s.chrome_first + s.chrome_n; ++c) {
            const Chrome& e = d.chrome[c];
            if (static_cast<uint8_t>(e.shape) >= static_cast<uint8_t>(Shape::kCount))
                return fail(why, "unknown shape", c);
            if (e.scale < 1 || e.scale > 12) return fail(why, "text scale", c);
            if (e.align > 2) return fail(why, "text align", c);
            if (e.shape == Shape::Text && !good_text(e.text, kTextLen, true)) return fail(why, "text", c);
            if (e.shape == Shape::Icon) {
                const bool builtin = e.icon < kIconCount;
                const bool user = e.icon >= kUserImage && e.icon - kUserImage < d.image_count;
                if (!builtin && !user) return fail(why, "unknown icon", c);
            }
            if (e.shape == Shape::Arc && e.a1 <= e.a0) return fail(why, "arc angles", c);
            if (e.w < -kGlassW * 2 || e.w > kGlassW * 2 || e.h < -kGlassH * 2 || e.h > kGlassH * 2)
                return fail(why, "shape size", c);
        }

        int sparks = 0;
        for (int r = s.readout_first; r < s.readout_first + s.readout_n; ++r) {
            const Readout& o = d.readouts[r];
            if (static_cast<uint8_t>(o.style) >= static_cast<uint8_t>(Style::kCount))
                return fail(why, "unknown readout style", r);
            if (o.style == Style::Spark && ++sparks > kMaxSparksPerScreen)
                return fail(why, "too many traces on a screen", r);
            if (o.w < 4 || o.h < 4) return fail(why, "readout too small", r);
            if (o.x < 0 || o.y < top || o.x + o.w > kGlassW || o.y + o.h > kGlassH)
                return fail(why, o.y < top ? "readout under the title" : "readout off the glass", r);
            if (o.scale < 1 || o.scale > 12) return fail(why, "readout scale", r);
            if (o.decimals < -1 || o.decimals > 6) return fail(why, "decimals", r);
            if (o.align > 2) return fail(why, "readout align", r);
            if (!good_text(o.caption, kCaptionLen, true)) return fail(why, "caption", r);
            if (!std::isfinite(o.lo) || !std::isfinite(o.hi) || !std::isfinite(o.warn) ||
                !std::isfinite(o.alarm))
                return fail(why, "range not a number", r);
            if ((o.style == Style::Bar || o.style == Style::Gauge || o.style == Style::Spark) &&
                !(o.hi > o.lo))
                return fail(why, "range: hi must be above lo", r);
            (void)k.field;   // an unknown field is allowed: drawn as "?"

            const Box rb{o.x, o.y, static_cast<int16_t>(o.x + o.w), static_cast<int16_t>(o.y + o.h)};
            for (int r2 = s.readout_first; r2 < r; ++r2) {
                const Readout& p = d.readouts[r2];
                const Box pb{p.x, p.y, static_cast<int16_t>(p.x + p.w), static_cast<int16_t>(p.y + p.h)};
                if (overlaps(rb, pb)) return fail(why, "two readouts overlap", r);
            }
            // Chrome under a readout is wiped the first time the value
            // changes: a readout repaints its whole box. A shape that holds
            // the readout inside it (a panel behind a value) is fine.
            for (int c = s.chrome_first; c < s.chrome_first + s.chrome_n; ++c) {
                const Chrome& e = d.chrome[c];
                Box cb{};
                chrome_box(e, cb.x0, cb.y0, cb.x1, cb.y1);
                if (!overlaps(cb, rb)) continue;
                const bool panel = e.shape == Shape::Rect || e.shape == Shape::FillRect ||
                                   e.shape == Shape::RoundRect || e.shape == Shape::FillRoundRect ||
                                   e.shape == Shape::Circle || e.shape == Shape::Disc;
                if (panel && contains(cb, rb)) continue;
                return fail(why, "a shape crosses a readout", r);
            }
        }
    }
    return true;
}

// --- the .dui format ---------------------------------------------------------

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

namespace {
constexpr uint8_t kMagic[4] = {'D', 'U', 'I', '1'};
constexpr uint8_t kVersion = 1;

struct Writer {
    uint8_t* p; size_t n; size_t at = 0; bool ok = true;
    void u8(uint8_t v) { if (at < n) p[at] = v; else ok = false; ++at; }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v >> 16)); }
    void f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    void bytes(const void* s, size_t len) {
        for (size_t i = 0; i < len; ++i) u8(static_cast<const uint8_t*>(s)[i]);
    }
    void str(const char* s, size_t cap) {
        const size_t len = strnlen(s, cap - 1);
        u8(static_cast<uint8_t>(len));
        bytes(s, len);
    }
};

struct Reader {
    const uint8_t* p; size_t n; size_t at = 0; bool ok = true;
    uint8_t u8() { if (at >= n) { ok = false; return 0; } return p[at++]; }
    uint16_t u16() { const uint16_t a = u8(); return static_cast<uint16_t>(a | (u8() << 8)); }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() { const uint32_t a = u16(); return a | (static_cast<uint32_t>(u16()) << 16); }
    float f32() { const uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }
    void bytes(void* d, size_t len) {
        for (size_t i = 0; i < len; ++i) static_cast<uint8_t*>(d)[i] = u8();
    }
    // A string written by Writer::str into a `cap`-byte array, NUL-terminated.
    void str(char* d, size_t cap) {
        const uint8_t len = u8();
        if (len >= cap) { ok = false; return; }
        bytes(d, len);
        d[len] = '\0';
    }
};
}  // namespace

size_t encode(const Deck& d, uint8_t* out, size_t n) {
    Writer w{out, n};
    w.bytes(kMagic, 4);
    w.u8(kVersion);
    w.u8(d.mode_count);
    w.u8(d.screen_count);
    w.u8(d.image_count);
    w.u16(d.chrome_count);
    w.u16(d.readout_count);
    w.u16(d.image_bytes);
    for (int i = 0; i < d.mode_count; ++i) {
        const ModeDef& m = d.modes[i];
        w.str(m.name, kNameLen);
        w.u8(m.count);
        w.bytes(m.screens, m.count);
    }
    for (int i = 0; i < d.screen_count; ++i) {
        const Screen& s = d.screens[i];
        w.str(s.name, kNameLen);
        w.u8(s.flags);
        w.u16(s.background);
        w.u16(s.e_action);
        w.u16(s.chrome_first);
        w.u8(s.chrome_n);
        w.u16(s.readout_first);
        w.u8(s.readout_n);
    }
    for (int i = 0; i < d.image_count; ++i) {
        w.u8(d.images[i].w);
        w.u8(d.images[i].h);
        w.u16(d.images[i].offset);
    }
    for (int i = 0; i < d.chrome_count; ++i) {
        const Chrome& c = d.chrome[i];
        w.u8(static_cast<uint8_t>(c.shape));
        w.u8(c.radius); w.u8(c.scale); w.u8(c.align);
        w.u16(c.color);
        w.i16(c.x); w.i16(c.y); w.i16(c.w); w.i16(c.h);
        w.i16(c.x3); w.i16(c.y3); w.i16(c.a0); w.i16(c.a1);
        w.u8(c.icon);
        w.str(c.text, kTextLen);
    }
    for (int i = 0; i < d.readout_count; ++i) {
        const Readout& r = d.readouts[i];
        w.u16(r.field);
        w.u8(static_cast<uint8_t>(r.style));
        w.u8(r.scale);
        w.u8(static_cast<uint8_t>(r.decimals));
        w.u8(r.flags);
        w.u8(r.align);
        w.i16(r.x); w.i16(r.y); w.i16(r.w); w.i16(r.h);
        w.u16(r.ink); w.u16(r.ground);
        w.f32(r.lo); w.f32(r.hi); w.f32(r.warn); w.f32(r.alarm);
        w.str(r.caption, kCaptionLen);
    }
    w.bytes(d.image_bits, d.image_bytes);
    if (!w.ok) return 0;
    const uint32_t crc = crc32(out, w.at);
    w.u32(crc);
    return w.ok ? w.at : 0;
}

bool decode(const uint8_t* in, size_t n, Deck& d, Error* why, const Known& k) {
    if (n < 4 + 4 || n > kMaxEncoded) return fail(why, "not a layout file");
    if (std::memcmp(in, kMagic, 4) != 0) return fail(why, "not a layout file");
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored |= static_cast<uint32_t>(in[n - 4 + i]) << (8 * i);
    if (crc32(in, n - 4) != stored) return fail(why, "damaged (CRC)");

    Reader r{in, n - 4};
    r.at = 4;
    if (r.u8() != kVersion) return fail(why, "made by a newer firmware");
    d = Deck{};
    d.mode_count = r.u8();
    d.screen_count = r.u8();
    d.image_count = r.u8();
    d.chrome_count = r.u16();
    d.readout_count = r.u16();
    d.image_bytes = r.u16();
    if (d.mode_count > kMaxModes || d.screen_count > kMaxCustom || d.image_count > kMaxImages ||
        d.chrome_count > kMaxChrome || d.readout_count > kMaxReadouts || d.image_bytes > kImagePool)
        return fail(why, "over the limits");

    for (int i = 0; i < d.mode_count; ++i) {
        ModeDef& m = d.modes[i];
        r.str(m.name, kNameLen);
        m.count = r.u8();
        if (m.count > kMaxScreensPerMode) return fail(why, "too many screens in a mode", i);
        r.bytes(m.screens, m.count);
    }
    for (int i = 0; i < d.screen_count; ++i) {
        Screen& s = d.screens[i];
        r.str(s.name, kNameLen);
        s.flags = r.u8();
        s.background = r.u16();
        s.e_action = r.u16();
        s.chrome_first = r.u16();
        s.chrome_n = r.u8();
        s.readout_first = r.u16();
        s.readout_n = r.u8();
    }
    for (int i = 0; i < d.image_count; ++i) {
        d.images[i].w = r.u8();
        d.images[i].h = r.u8();
        d.images[i].offset = r.u16();
    }
    for (int i = 0; i < d.chrome_count; ++i) {
        Chrome& c = d.chrome[i];
        c.shape = static_cast<Shape>(r.u8());
        c.radius = r.u8(); c.scale = r.u8(); c.align = r.u8();
        c.color = r.u16();
        c.x = r.i16(); c.y = r.i16(); c.w = r.i16(); c.h = r.i16();
        c.x3 = r.i16(); c.y3 = r.i16(); c.a0 = r.i16(); c.a1 = r.i16();
        c.icon = r.u8();
        r.str(c.text, kTextLen);
    }
    for (int i = 0; i < d.readout_count; ++i) {
        Readout& o = d.readouts[i];
        o.field = r.u16();
        o.style = static_cast<Style>(r.u8());
        o.scale = r.u8();
        o.decimals = static_cast<int8_t>(r.u8());
        o.flags = r.u8();
        o.align = r.u8();
        o.x = r.i16(); o.y = r.i16(); o.w = r.i16(); o.h = r.i16();
        o.ink = r.u16(); o.ground = r.u16();
        o.lo = r.f32(); o.hi = r.f32(); o.warn = r.f32(); o.alarm = r.f32();
        r.str(o.caption, kCaptionLen);
    }
    r.bytes(d.image_bits, d.image_bytes);
    if (!r.ok) return fail(why, "cut short");
    if (r.at != r.n) return fail(why, "extra bytes");
    return validate(d, why, k);
}

}  // namespace core::layout
