// Field lookup and formatting for custom screens. The table itself is
// layout_fields.gen.cpp, generated from the Frame's headers.
#include "core/layout/fields.hpp"
#include "core/state/slots.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace core::layout {

const FieldDef* find_field(uint16_t id) {
    // Ids are assigned in table order and only go up, so a binary search
    // works; the table is ~500 entries and this runs once a screen build.
    const FieldDef* f = fields();
    size_t lo = 0, hi = field_count();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (f[mid].id == id) return &f[mid];
        if (f[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

const char* enum_name(const FieldDef& f, int v) {
    if (!f.names) return nullptr;
    for (size_t i = 0; i < f.names->count; ++i) {
        if (f.names->items[i].value == v) return f.names->items[i].name;
    }
    return nullptr;
}

namespace {
void put(Shown& s, const char* t) {
    std::snprintf(s.text, sizeof s.text, "%s", t);
}
}  // namespace

Shown show(const FieldDef& f, const state::Frame& fr, const ShowCtx& c, int decimals) {
    Shown s;
    const Value v = f.get(fr);
    s.number = v.num;
    s.unit = f.unit;
    s.valid = f.dev < 0 || fr.health.working(static_cast<state::Dev>(f.dev));
    const int dec = decimals >= 0 ? decimals : f.decimals;

    switch (v.kind) {
        case Value::Str:
            put(s, v.str ? v.str : "");
            s.unit = "";
            return s;
        case Value::Bool:
            put(s, v.num != 0.0 ? "ON" : "OFF");
            s.unit = "";
            return s;
        case Value::Enum: {
            const char* n = enum_name(f, static_cast<int>(v.num));
            if (n) put(s, n);
            else std::snprintf(s.text, sizeof s.text, "%d", static_cast<int>(v.num));
            s.unit = "";
            return s;
        }
        case Value::Num:
            break;
    }

    float x = static_cast<float>(v.num);
    switch (f.q) {
        case Quantity::Speed:    x = units::speed_for_display(x, c.units, &s.unit); break;
        case Quantity::DistMi:   x = units::distance_for_display(x, c.units, &s.unit); break;
        case Quantity::DistFt:
            if (c.units == units::System::Metric) { x = units::m_from_ft(x); s.unit = "m"; }
            break;
        case Quantity::AltM:
            if (c.units == units::System::Imperial) { x = units::ft_from_m(x); s.unit = "ft"; }
            break;
        case Quantity::Temp:     x = units::temp_for_display(x, c.units, &s.unit); break;
        case Quantity::Pressure: x = units::pressure_for_display(x, c.units, &s.unit); break;
        case Quantity::Volume:   x = units::volume_for_display(x, c.units, &s.unit); break;
        case Quantity::Rpm:      x = units::rpm_for_display(x); break;
        case Quantity::Duration:
            units::fmt_duration(s.text, sizeof s.text, static_cast<uint32_t>(v.num),
                                s.valid && v.num > 0.0);
            s.unit = "";
            return s;
        case Quantity::Seconds: {
            const uint32_t t = static_cast<uint32_t>(v.num);
            if (t >= 3600) std::snprintf(s.text, sizeof s.text, "%u:%02u:%02u",
                                         static_cast<unsigned>(t / 3600),
                                         static_cast<unsigned>(t / 60 % 60),
                                         static_cast<unsigned>(t % 60));
            else std::snprintf(s.text, sizeof s.text, "%u:%02u",
                               static_cast<unsigned>(t / 60), static_cast<unsigned>(t % 60));
            s.unit = "";
            return s;
        }
        case Quantity::Clock:
            units::fmt_clock(s.text, sizeof s.text, static_cast<uint32_t>(v.num),
                             c.tz_offset_h, s.valid && fr.gnss.fix, c.clock_12h);
            s.unit = "";
            return s;
        case Quantity::Plain:
            break;
    }
    if (!std::isfinite(x)) s.valid = false;
    units::fmt_value(s.text, sizeof s.text, x, dec, s.valid);
    return s;
}

}  // namespace core::layout
