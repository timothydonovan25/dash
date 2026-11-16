// The screen editor's view of the firmware (services/editor_info.hpp).
#include "services/editor_info.hpp"
#include "core/config/config.hpp"
#include "core/layout/deck.hpp"
#include "core/state/slots.hpp"
#include "ui/canvas.hpp"
#include "ui/icons.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace services {

namespace lay = core::layout;

namespace {
void out(EmitFn emit, void* ctx, const char* s) { emit(s, std::strlen(s), ctx); }

// A JSON string body: quotes and backslashes escaped, control bytes dropped.
void str(EmitFn emit, void* ctx, const char* s) {
    char buf[96];
    size_t n = 0;
    buf[n++] = '"';
    for (; s && *s; ++s) {
        if (n > sizeof buf - 4) { emit(buf, n, ctx); n = 0; }
        if (*s == '"' || *s == '\\') buf[n++] = '\\';
        if (static_cast<unsigned char>(*s) >= 0x20) buf[n++] = *s;
    }
    buf[n++] = '"';
    emit(buf, n, ctx);
}

void hex(EmitFn emit, void* ctx, const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    char buf[64];
    size_t k = 0;
    buf[k++] = '"';
    for (size_t i = 0; i < n; ++i) {
        if (k > sizeof buf - 3) { emit(buf, k, ctx); k = 0; }
        buf[k++] = kHex[p[i] >> 4];
        buf[k++] = kHex[p[i] & 15];
    }
    buf[k++] = '"';
    emit(buf, k, ctx);
}

const char* type_name(lay::Type t) {
    switch (t) {
        case lay::Type::Num:  return "num";
        case lay::Type::Int:  return "int";
        case lay::Type::Bool: return "bool";
        case lay::Type::Str:  return "str";
        case lay::Type::Enum: return "enum";
    }
    return "num";
}
}  // namespace

void write_editor_info(EmitFn emit, void* ctx) {
    char buf[160];
    const ui::Font& font = ui::font_5x7();
    out(emit, ctx, "{\"font\":{\"w\":");
    std::snprintf(buf, sizeof buf, "%u,\"h\":%u,\"first\":%u,\"last\":%u,\"advance\":%u,\"glyphs\":",
                  font.width, font.height, font.first, font.last, font.advance);
    out(emit, ctx, buf);
    hex(emit, ctx, font.glyphs, static_cast<size_t>(font.last - font.first + 1) * font.width);

    out(emit, ctx, "},\"icons\":[");
    const ui::icon::Bitmap* icons[] = {&ui::icon::kSong, &ui::icon::kAlbum, &ui::icon::kArtist,
                                       &ui::icon::kLyric};
    static_assert(sizeof icons / sizeof icons[0] == lay::kIconCount, "an icon for each index");
    const char* icon_names[] = {"song", "album", "artist", "lyric"};
    for (size_t i = 0; i < lay::kIconCount; ++i) {
        std::snprintf(buf, sizeof buf, "%s{\"name\":\"%s\",\"w\":%d,\"h\":%d,\"bits\":", i ? "," : "",
                      icon_names[i], icons[i]->w, icons[i]->h);
        out(emit, ctx, buf);
        hex(emit, ctx, icons[i]->bits, static_cast<size_t>((icons[i]->w + 7) / 8) * icons[i]->h);
        out(emit, ctx, "}");
    }

    // Every built-in by number; the editor offers kFirstRiding..kLastRiding.
    out(emit, ctx, "],\"builtins\":[");
    for (uint8_t id = 0; id < lay::kBuiltinEnd; ++id) {
        if (id) out(emit, ctx, ",");
        str(emit, ctx, id ? lay::builtin_name(id) : "");
    }
    std::snprintf(buf, sizeof buf, "],\"riding\":[%u,%u],\"actions\":[", lay::kFirstRiding, lay::kLastRiding);
    out(emit, ctx, buf);
    bool first = true;
    for (size_t i = 0; i < core::config::item_count(); ++i) {
        const core::config::ConfigItem& it = core::config::items()[i];
        if (it.kind != core::config::Kind::Action) continue;
        std::snprintf(buf, sizeof buf, "%s{\"key\":%u,\"confirm\":%d,\"label\":", first ? "" : ",",
                      it.key, it.confirm ? 1 : 0);
        out(emit, ctx, buf);
        str(emit, ctx, it.label);
        out(emit, ctx, ",\"group\":");
        str(emit, ctx, it.group);
        out(emit, ctx, "}");
        first = false;
    }

    out(emit, ctx, "],\"fields\":[");
    for (size_t i = 0; i < lay::field_count(); ++i) {
        const lay::FieldDef& f = lay::fields()[i];
        std::snprintf(buf, sizeof buf, "%s{\"id\":%u,\"t\":\"%s\",\"q\":%u,\"d\":%u,\"p\":", i ? "," : "",
                      f.id, type_name(f.type), static_cast<unsigned>(f.q), f.decimals);
        out(emit, ctx, buf);
        str(emit, ctx, f.path);
        out(emit, ctx, ",\"c\":");
        str(emit, ctx, f.caption);
        if (f.unit[0]) { out(emit, ctx, ",\"u\":"); str(emit, ctx, f.unit); }
        if (f.column[0]) { out(emit, ctx, ",\"col\":"); str(emit, ctx, f.column); }
        if (f.names) {
            out(emit, ctx, ",\"e\":[");
            for (size_t k = 0; k < f.names->count; ++k) {
                if (k) out(emit, ctx, ",");
                str(emit, ctx, f.names->items[k].name);
            }
            out(emit, ctx, "]");
        }
        out(emit, ctx, "}");
    }
    out(emit, ctx, "]}");
}

void write_values(const core::state::Frame& f, const lay::ShowCtx& c, const uint16_t* ids,
                  size_t n, EmitFn emit, void* ctx) {
    out(emit, ctx, "[");
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        const lay::FieldDef* d = lay::find_field(ids[i]);
        if (!d) continue;
        const lay::Shown s = lay::show(*d, f, c);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s{\"id\":%u,\"v\":%d,\"n\":%.6g,\"t\":", first ? "" : ",", d->id,
                      s.valid ? 1 : 0, s.number);
        out(emit, ctx, buf);
        str(emit, ctx, s.text);
        out(emit, ctx, ",\"u\":");
        str(emit, ctx, s.unit);
        out(emit, ctx, "}");
        first = false;
    }
    out(emit, ctx, "]");
}

size_t parse_ids(const char* s, uint16_t* ids, size_t max) {
    size_t n = 0;
    while (s && *s && n < max) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 10);
        if (end == s) break;
        if (v > 0 && v <= 0xFFFF) ids[n++] = static_cast<uint16_t>(v);
        s = *end == ',' ? end + 1 : end;
        if (*end != ',') break;
    }
    return n;
}

}  // namespace services
