#include "core/config/config.hpp"
#include <cstddef>

namespace core::config {
namespace {

const char* const kUnitOptions[] = {"Imperial", "Metric"};

constexpr uint16_t kNoOffset = 0xFFFF;

constexpr ConfigItem num(uint16_t key, const char* label, const char* group,
                         Kind kind, float mn, float mx, float step,
                         const char* unit, uint16_t offset, uint8_t width) {
    return ConfigItem{key, label, group, kind, mn, mx, step, unit, offset,
                      width, nullptr, 0, state::CommandId::None};
}

constexpr ConfigItem boolean(uint16_t key, const char* label,
                             const char* group, uint16_t offset,
                             uint8_t width) {
    return ConfigItem{key, label, group, Kind::Bool, 0, 1, 1, "", offset,
                      width, nullptr, 0, state::CommandId::None};
}

constexpr ConfigItem choice(uint16_t key, const char* label, const char* group,
                            uint16_t offset, uint8_t width,
                            const char* const* opts, uint8_t count) {
    return ConfigItem{key, label, group, Kind::Enum, 0,
                      static_cast<float>(count - 1), 1, "", offset, width,
                      opts, count, state::CommandId::None};
}

constexpr ConfigItem action(uint16_t key, const char* label, const char* group,
                            state::CommandId id) {
    return ConfigItem{key, label, group, Kind::Action, 0, 0, 0, "", kNoOffset,
                      0, nullptr, 0, id};
}

// Offset and width always come from the same member name, so they cannot
// disagree with each other.
#define FIELD(f) static_cast<uint16_t>(offsetof(Settings, f)), \
                 static_cast<uint8_t>(sizeof(Settings::f))

// One element of the gear table. Offsetting into the array by hand is the
// price of keeping it an array, which the estimator wants to index.
#define GEAR(i) static_cast<uint16_t>(offsetof(Settings, gear_rpm_per_mph) + \
                                      (i) * sizeof(float)), 4

// Keys are grouped by hundred for readability only. They are stable
// identifiers, not indices — the order of this array may change freely, the
// numbers may not.
const ConfigItem kItems[] = {
    // Display
    num   (0x0101, "Brightness",   "Display", Kind::Int,  5, 100, 5, "%",  FIELD(brightness_pct)),
    choice(0x0102, "Units",        "Display", FIELD(unit_system), kUnitOptions, 2),
    num   (0x0103, "UTC offset",   "Display", Kind::Int, -12, 14, 1, "h",  FIELD(tz_offset_h)),
    boolean(0x0104,"Dim at night", "Display", FIELD(dim_at_night)),

    // Wheels
    num(0x0201, "Front circ.",  "Wheels", Kind::Int, 1000, 3000, 5, "mm", FIELD(front_circumference_mm)),
    num(0x0202, "Rear circ.",   "Wheels", Kind::Int, 1000, 3000, 5, "mm", FIELD(rear_circumference_mm)),
    num(0x0203, "Front pulses", "Wheels", Kind::Int,    1,   32, 1, "/rev", FIELD(front_pulses_per_rev)),
    num(0x0204, "Rear pulses",  "Wheels", Kind::Int,    1,   32, 1, "/rev", FIELD(rear_pulses_per_rev)),

    // Shift lights
    num(0x0301, "First lamp",  "Shift", Kind::Int, 2000, 12000, 100, "rpm", FIELD(shift_base_rpm)),
    num(0x0302, "Span",        "Shift", Kind::Int,  200,  6000, 100, "rpm", FIELD(shift_span_rpm)),
    num(0x0303, "Hysteresis",  "Shift", Kind::Int,    0,   500,  10, "rpm", FIELD(shift_hyst_rpm)),
    num(0x0304, "Flash above", "Shift", Kind::Int, 3000, 14000, 100, "rpm", FIELD(shift_flash_rpm)),

    // Lap timer
    num(0x0401, "Line width",   "Lap", Kind::Float, 10, 200,  5, "ft",  FIELD(lap_half_width_ft)),
    num(0x0402, "Arm speed",    "Lap", Kind::Float,  0,  60,  1, "mph", FIELD(lap_min_speed_mph)),
    num(0x0403, "Min lap",      "Lap", Kind::Int,    2, 600,  1, "s",   FIELD(lap_min_lap_s)),
    action(0x0404, "Set line here", "Lap", state::CommandId::LapSetLine),
    action(0x0405, "Clear laps",    "Lap", state::CommandId::LapClear),

    // Gearing. One ratio per gear, in rpm per mph. Calibrate against the
    // ratio the DIAG screen shows at a steady speed in that gear.
    num(0x0801, "Gears",   "Gearing", Kind::Int, 1, 6, 1, "", FIELD(gear_count)),
    num(0x0802, "1st",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(0)),
    num(0x0803, "2nd",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(1)),
    num(0x0804, "3rd",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(2)),
    num(0x0805, "4th",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(3)),
    num(0x0806, "5th",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(4)),
    num(0x0807, "6th",     "Gearing", Kind::Float,  0, 900, 5, "r/mph", GEAR(5)),

    // Fuel
    num(0x0501, "Idle burn", "Fuel", Kind::Float, 0.02f, 2.0f, 0.01f, "gph", FIELD(fuel_idle_gph)),
    num(0x0502, "WOT burn",  "Fuel", Kind::Float, 0.10f, 8.0f, 0.05f, "gph", FIELD(fuel_wot_gph)),
    num(0x0503, "Tank",      "Fuel", Kind::Float, 0.20f, 10.0f, 0.05f,"gal", FIELD(fuel_tank_gal)),

    // IMU
    num(0x0601, "Roll offset",  "IMU", Kind::Float, -45, 45, 0.5f, "deg", FIELD(imu_roll_offset_deg)),
    num(0x0602, "Pitch offset", "IMU", Kind::Float, -45, 45, 0.5f, "deg", FIELD(imu_pitch_offset_deg)),
    action(0x0603, "Calibrate level", "IMU", state::CommandId::ImuCalibrate),

    // Trip
    action(0x0701, "Reset trip A", "Trip", state::CommandId::TripReset),

    // Phone (§11.3). Forget drops every bond, so the next phone pairs
    // without a window; pairing mode lets one more phone bond for a minute.
    action(0x0A01, "Forget phone", "Phone", state::CommandId::BleForgetPhone),
    action(0x0A02, "Pairing mode", "Phone", state::CommandId::BlePairingMode),

    // Blackbox. 50 Hz is about 27 MB an hour and gives the ring ten seconds
    // of slack against an SD stall; 5 Hz is a hundred. Lap markers are
    // written whatever this says.
    choice(0x0901, "Rate", "Log", FIELD(log_rate),
           core::log::kRateLabels, core::log::kRateCount),
};

#undef FIELD
#undef GEAR

constexpr size_t kCount = sizeof(kItems) / sizeof(kItems[0]);

// The offset is into a Settings the caller owns; these two are the only
// places that reinterpret it, so the casting is contained.
const void* field(const Settings& s, const ConfigItem& it) {
    return reinterpret_cast<const char*>(&s) + it.offset;
}
void* field(Settings& s, const ConfigItem& it) {
    return reinterpret_cast<char*>(&s) + it.offset;
}

size_t emit_lit(char* buf, size_t n, const char* s) {
    if (n == 0) return 0;
    size_t len = 0;
    while (s[len]) ++len;
    const size_t w = len < n - 1 ? len : n - 1;
    for (size_t i = 0; i < w; ++i) buf[i] = s[i];
    buf[w] = '\0';
    return w;
}

}  // namespace

const ConfigItem* items()     { return kItems; }
size_t            item_count(){ return kCount; }

const ConfigItem* find(uint16_t key) {
    for (size_t i = 0; i < kCount; ++i) {
        if (kItems[i].key == key) return &kItems[i];
    }
    return nullptr;
}

float clamp(const ConfigItem& it, float v) {
    if (it.kind == Kind::Action) return 0.0f;
    if (v < it.min) return it.min;
    if (v > it.max) return it.max;
    return v;
}

float get(const Settings& s, const ConfigItem& it) {
    if (it.offset == kNoOffset) return 0.0f;
    const void* p = field(s, it);
    switch (it.kind) {
        case Kind::Float: return *static_cast<const float*>(p);
        case Kind::Bool:  return *static_cast<const bool*>(p) ? 1.0f : 0.0f;
        case Kind::Enum:  return static_cast<float>(*static_cast<const uint8_t*>(p));
        case Kind::Int:   break;
        case Kind::Action:return 0.0f;
    }
    // Int items keep Settings compact — a pulse count does not need four
    // bytes. Width comes from the member, signedness from the range.
    const bool sign = it.min < 0.0f;
    switch (it.width) {
        case 1: return sign ? *static_cast<const int8_t*>(p)
                            : *static_cast<const uint8_t*>(p);
        case 2: return sign ? *static_cast<const int16_t*>(p)
                            : *static_cast<const uint16_t*>(p);
        default: break;
    }
    return sign ? static_cast<float>(*static_cast<const int32_t*>(p))
                : static_cast<float>(*static_cast<const uint32_t*>(p));
}

bool set(Settings& s, const ConfigItem& it, float v) {
    if (it.kind == Kind::Action || it.offset == kNoOffset) return false;
    const float before = get(s, it);
    const float c = clamp(it, v);
    void* p = field(s, it);
    switch (it.kind) {
        case Kind::Float: *static_cast<float*>(p) = c; break;
        case Kind::Bool:  *static_cast<bool*>(p) = c >= 0.5f; break;
        case Kind::Enum:  *static_cast<uint8_t*>(p) = static_cast<uint8_t>(c + 0.5f); break;
        case Kind::Int: {
            const int32_t rounded = static_cast<int32_t>(c < 0 ? c - 0.5f : c + 0.5f);
            const bool sign = it.min < 0.0f;
            switch (it.width) {
                case 1:
                    if (sign) *static_cast<int8_t*>(p) = static_cast<int8_t>(rounded);
                    else      *static_cast<uint8_t*>(p) = static_cast<uint8_t>(rounded);
                    break;
                case 2:
                    if (sign) *static_cast<int16_t*>(p) = static_cast<int16_t>(rounded);
                    else      *static_cast<uint16_t*>(p) = static_cast<uint16_t>(rounded);
                    break;
                default:
                    *static_cast<int32_t*>(p) = rounded;
                    break;
            }
            break;
        }
        case Kind::Action: return false;
    }
    return get(s, it) != before;
}

bool nudge(Settings& s, const ConfigItem& it, int direction) {
    if (direction == 0 || it.kind == Kind::Action) return false;
    if (it.kind == Kind::Bool) {
        return set(s, it, get(s, it) >= 0.5f ? 0.0f : 1.0f);
    }
    if (it.kind == Kind::Enum) {
        // Enums wrap: pressing down on the last option is how you get back.
        const int count = it.option_count > 0 ? it.option_count : 1;
        int v = static_cast<int>(get(s, it)) + (direction > 0 ? 1 : -1);
        if (v < 0) v = count - 1;
        if (v >= count) v = 0;
        return set(s, it, static_cast<float>(v));
    }
    return set(s, it, get(s, it) + (direction > 0 ? it.step : -it.step));
}

size_t format(const Settings& s, const ConfigItem& it, char* buf, size_t n) {
    if (it.kind == Kind::Action) return emit_lit(buf, n, "press E");
    const float v = get(s, it);
    if (it.kind == Kind::Bool) return emit_lit(buf, n, v >= 0.5f ? "On" : "Off");
    if (it.kind == Kind::Enum) {
        const uint8_t i = static_cast<uint8_t>(v);
        if (it.options && i < it.option_count) return emit_lit(buf, n, it.options[i]);
        return emit_lit(buf, n, "?");
    }

    // One decimal is enough for anything with a fractional step; counts and
    // rpm print whole.
    const int decimals = (it.kind == Kind::Float && it.step < 1.0f) ? 2 : 0;
    size_t k = units::fmt_float(buf, n, v, decimals);
    if (it.unit && it.unit[0] && k + 2 < n) {
        buf[k++] = ' ';
        k += emit_lit(buf + k, n - k, it.unit);
    }
    return k;
}

ShiftView shift_view(const Settings& s) {
    return ShiftView{s.shift_base_rpm, s.shift_span_rpm, s.shift_hyst_rpm,
                     s.shift_flash_rpm};
}

core::gear::Config gear_view(const Settings& s) {
    core::gear::Config g;
    g.count = s.gear_count > core::gear::kMaxGears ? core::gear::kMaxGears
                                                   : s.gear_count;
    for (uint8_t i = 0; i < core::gear::kMaxGears; ++i) {
        g.rpm_per_mph[i] = s.gear_rpm_per_mph[i];
    }
    return g;
}

LapView lap_view(const Settings& s) {
    return LapView{static_cast<double>(s.lap_half_width_ft),
                   s.lap_min_speed_mph,
                   static_cast<uint32_t>(s.lap_min_lap_s) * 1000u};
}

bool validate_table(const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    for (size_t i = 0; i < kCount; ++i) {
        const ConfigItem& a = kItems[i];
        if (a.key == 0)              { *why = "key 0 is reserved"; return false; }
        if (!a.label || !a.group)    { *why = "item without a label or group"; return false; }
        for (size_t j = i + 1; j < kCount; ++j) {
            if (kItems[j].key == a.key) { *why = "duplicate key"; return false; }
        }
        if (a.kind == Kind::Action) {
            if (a.offset != kNoOffset)               { *why = "action with a field"; return false; }
            if (a.action == state::CommandId::None)  { *why = "action with no command"; return false; }
            continue;
        }
        if (a.offset == kNoOffset || a.offset + a.width > sizeof(Settings)) {
            *why = "field outside Settings"; return false;
        }
        if (a.width != 1 && a.width != 2 && a.width != 4) {
            *why = "field is not 1, 2 or 4 bytes"; return false;
        }
        if (a.kind == Kind::Float && a.width != 4) { *why = "float in a narrow field"; return false; }
        if (a.kind == Kind::Bool  && a.width != 1) { *why = "bool in a wide field"; return false; }
        if (a.kind == Kind::Enum  && a.width != 1) { *why = "enum in a wide field"; return false; }
        if (a.min > a.max)  { *why = "min above max"; return false; }
        if (a.step <= 0.0f) { *why = "step must be positive"; return false; }
        if (a.kind == Kind::Enum && (!a.options || a.option_count == 0)) {
            *why = "enum without options"; return false;
        }
        // The range has to fit the field it is stored in. Without this, an
        // item declared 0..3000 over a uint8_t would write 3000 & 0xFF and
        // the setting would quietly wrap instead of clamping.
        if (a.kind == Kind::Int) {
            const bool sign = a.min < 0.0f;
            const double lim = sign ? (1u << (a.width * 8 - 1))
                                    : (a.width >= 4 ? 4294967296.0
                                                    : static_cast<double>(1u << (a.width * 8)));
            const double lo = sign ? -lim : 0.0;
            const double hi = sign ? lim - 1.0 : lim - 1.0;
            if (a.min < lo || a.max > hi) { *why = "range does not fit the field"; return false; }
        }
    }
    return true;
}

}  // namespace core::config
