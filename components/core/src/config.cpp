#include "core/config/config.hpp"
#include <cstddef>
#include <cstring>

namespace core::config {
namespace {

const char* const kUnitOptions[] = {"Imperial", "Metric"};
const char* const kSpeedoOptions[] = {"Wheels", "Fused"};
const char* const kPaletteOptions[] = {"Standard", "Contrast"};
// In kFrameRateHz's order (config.hpp).
const char* const kFrameRateOptions[] = {"10 Hz", "20 Hz", "30 Hz", "60 Hz", "120 Hz", "Unlimited"};
static_assert(sizeof(kFrameRateOptions) / sizeof(kFrameRateOptions[0]) == kFrameRateCount,
              "a frame-rate label per kFrameRateHz entry");

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

// An action that loses something: the screen wants E twice.
constexpr ConfigItem confirmed(uint16_t key, const char* label, const char* group,
                               state::CommandId id) {
    ConfigItem it = action(key, label, group, id);
    it.confirm = true;
    return it;
}

// What every setting starts as, for default_value(). Read-only.
const Settings kDefaults{};

// Offset and width always come from the same member name, so they cannot
// disagree with each other.
#define FIELD(f) static_cast<uint16_t>(offsetof(Settings, f)), \
                 static_cast<uint8_t>(sizeof(Settings::f))

// One element of the gear table. Offsetting into the array by hand is the
// price of keeping it an array, which the estimator wants to index.
#define GEAR(i) static_cast<uint16_t>(offsetof(Settings, gear_rpm_per_mph) + \
                                      (i) * sizeof(float)), 4

#define GBASE(i) static_cast<uint16_t>(offsetof(Settings, gear_base_rpm_per_mph) + \
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
    choice(0x0105, "Speedo",       "Display", FIELD(speedo_source), kSpeedoOptions, 2),
    boolean(0x0106,"Slip bar",     "Display", FIELD(slip_bar)),
    boolean(0x0107,"Popups",       "Display", FIELD(popups)),
    boolean(0x0108,"12h clock",    "Display", FIELD(clock_12h)),
    choice(0x0109, "Palette",      "Display", FIELD(palette), kPaletteOptions, 2),

    // Start-up sweep. Its own submenu so it is easy to find and turn off.
    boolean(0x0B01,"Gauge sweep",  "Startup", FIELD(gauge_sweep)),
    num   (0x0B02, "Sweep time",   "Startup", Kind::Int, 500, 5000, 100, "ms", FIELD(sweep_ms)),
    boolean(0x0B03,"Splash",       "Startup", FIELD(splash)),

    // Wheels
    num(0x0201, "Front circ.",  "Wheels", Kind::Int, 1000, 3000, 5, "mm", FIELD(front_circumference_mm)),
    num(0x0202, "Rear circ.",   "Wheels", Kind::Int, 1000, 3000, 5, "mm", FIELD(rear_circumference_mm)),
    num(0x0203, "Front pulses", "Wheels", Kind::Int,    1,   32, 1, "/rev", FIELD(front_pulses_per_rev)),
    num(0x0204, "Rear pulses",  "Wheels", Kind::Int,    1,   32, 1, "/rev", FIELD(rear_pulses_per_rev)),
    num(0x0205, "Smoothing",    "Wheels", Kind::Int,    0,  100, 5, "ms", FIELD(wheel_smooth_ms)),

    // Shift lights
    num(0x0301, "First lamp",  "Shift", Kind::Int, 2000, 12000, 100, "rpm", FIELD(shift_base_rpm)),
    num(0x0302, "Span",        "Shift", Kind::Int,  200,  6000, 100, "rpm", FIELD(shift_span_rpm)),
    num(0x0303, "Hysteresis",  "Shift", Kind::Int,    0,   500,  10, "rpm", FIELD(shift_hyst_rpm)),
    num(0x0304, "Flash above", "Shift", Kind::Int, 3000, 14000, 100, "rpm", FIELD(shift_flash_rpm)),
    num(0x0305, "Brightness",  "Shift", Kind::Int,    5,   100,   5, "%",   FIELD(shift_brightness_pct)),
    num(0x0306, "Revs/spark",  "Shift", Kind::Int,    1,     2,   1, "",    FIELD(tach_revs_per_pulse)),
    num(0x0307, "Lead time",   "Shift", Kind::Int,    0,   300,  10, "ms",  FIELD(shift_lead_ms)),
    num(0x0308, "Peak hold",   "Shift", Kind::Int,    0,  5000, 250, "ms",  FIELD(shift_peak_hold_ms)),
    num(0x0309, "Tach smooth", "Shift", Kind::Int,    0,    50,   1, "ms",  FIELD(tach_smooth_ms)),
    // The lamps run the start-up sweep once, to see them without revving.
    // `ctrl` stops it the moment the wheels turn, as it does the sweep.
    action(0x030A, "Test lamps",  "Shift", state::CommandId::ShiftLampTest),

    // Lap timer
    num(0x0401, "Line width",   "Lap", Kind::Float, 10, 200,  5, "ft",  FIELD(lap_half_width_ft)),
    num(0x0402, "Arm speed",    "Lap", Kind::Float,  0,  60,  1, "mph", FIELD(lap_min_speed_mph)),
    num(0x0403, "Min lap",      "Lap", Kind::Int,    2, 600,  1, "s",   FIELD(lap_min_lap_s)),
    action(0x0404, "Set line here", "Lap", state::CommandId::LapSetLine),
    confirmed(0x0405, "Clear laps", "Lap", state::CommandId::LapClear),
    // Deletes the saved track (core::track) whose line is set: its line,
    // splits, record and reference lap. The next visit starts from nothing.
    confirmed(0x0406, "Forget track", "Lap", state::CommandId::TrackForget),

    // Gearing. One ratio per gear, in rpm per mph. Calibrate against the
    // ratio the DIAG screen shows at a steady speed in that gear.
    num(0x0801, "Gears",   "Gearing", Kind::Int, 1, 6, 1, "", FIELD(gear_count)),
    num(0x0802, "1st",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(0)),
    num(0x0803, "2nd",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(1)),
    num(0x0804, "3rd",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(2)),
    num(0x0805, "4th",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(3)),
    num(0x0806, "5th",     "Gearing", Kind::Float, 40, 900, 5, "r/mph", GEAR(4)),
    num(0x0807, "6th",     "Gearing", Kind::Float,  0, 900, 5, "r/mph", GEAR(5)),
    // The calibration page: ride each gear steady for ten seconds, read the
    // ratio and its % error, accept. Auto-adjust is the learner.
    action(0x0808, "Calibrate gears", "Gearing", state::CommandId::GearCalStart),
    boolean(0x0809,"Auto-adjust",     "Gearing", FIELD(gear_autotune)),
    confirmed(0x080A, "Reset to base", "Gearing", state::CommandId::GearCalReset),
    // What the learner is held to. Written by the calibration and by typing
    // a ratio above; not meant to be edited.
    num(0x0811, "Base 1st", "Gear base", Kind::Float, 40, 900, 5, "r/mph", GBASE(0)),
    num(0x0812, "Base 2nd", "Gear base", Kind::Float, 40, 900, 5, "r/mph", GBASE(1)),
    num(0x0813, "Base 3rd", "Gear base", Kind::Float, 40, 900, 5, "r/mph", GBASE(2)),
    num(0x0814, "Base 4th", "Gear base", Kind::Float, 40, 900, 5, "r/mph", GBASE(3)),
    num(0x0815, "Base 5th", "Gear base", Kind::Float, 40, 900, 5, "r/mph", GBASE(4)),
    num(0x0816, "Base 6th", "Gear base", Kind::Float,  0, 900, 5, "r/mph", GBASE(5)),

    // Fuel
    num(0x0501, "Idle burn", "Fuel", Kind::Float, 0.02f, 2.0f, 0.01f, "gph", FIELD(fuel_idle_gph)),
    num(0x0502, "WOT burn",  "Fuel", Kind::Float, 0.10f, 8.0f, 0.05f, "gph", FIELD(fuel_wot_gph)),
    num(0x0503, "Tank",      "Fuel", Kind::Float, 0.20f, 10.0f, 0.05f,"gal", FIELD(fuel_tank_gal)),

    // IMU
    // Calibrate level writes these, unrounded. Pitch reaches 89: a dash on
    // the bars faces the rider, and clamping a 50° tilt would have stored a
    // rotation that was not the board's.
    num(kKeyImuRollOffset,  "Roll offset",  "IMU", Kind::Float, -45, 45, 0.5f, "deg", FIELD(imu_roll_offset_deg)),
    num(kKeyImuPitchOffset, "Pitch offset", "IMU", Kind::Float, -89, 89, 0.5f, "deg", FIELD(imu_pitch_offset_deg)),
    action(0x0603, "Calibrate level", "IMU", state::CommandId::ImuCalibrate),

    // Trip
    confirmed(0x0701, "Reset trip A", "Trip", state::CommandId::TripReset),

    // Phone (§11.3). Forget drops every bond, so the next phone pairs
    // without a window; pairing mode lets one more phone bond for a minute.
    confirmed(0x0A01, "Forget phone", "Phone", state::CommandId::BleForgetPhone),
    action(0x0A02, "Pairing mode", "Phone", state::CommandId::BlePairingMode),
    // Calls and messages from the phone, as a toast. The text itself only
    // when stopped unless asked for: a sender's name is a glance, a message
    // is reading.
    boolean(0x0A03,"Notifications", "Phone", FIELD(notify_on)),
    boolean(0x0A04,"Text moving",   "Phone", FIELD(notify_text_moving)),
    // A crash (core::crash) counts down 30 s, then asks the phone to text
    // for help. The phone has its own switch and holds the number.
    boolean(0x0A05,"Crash SOS",     "Phone", FIELD(crash_sos)),
    // The countdown now, as a crash would start it: the whole path to the
    // text, on the bench. Any button cancels it.
    confirmed(0x0A06, "Test crash", "Phone", state::CommandId::CrashTest),

    // Heart rate (core/hr): the watch's own broadcast, shown on STAT ->
    // HEART and logged. Turning it off and on forgets which watch it was.
    boolean(0x0F01,"Watch HR",  "Heart rate", FIELD(hr_on)),
    num   (0x0F02, "Max HR",    "Heart rate", Kind::Int, 120, 230, 1, "bpm", FIELD(hr_max_bpm)),

    // Routing (core::route, docs/navigation.md): keep routes off dirt.
    boolean(kKeyNavAvoidDirt,"Avoid dirt", "Nav", FIELD(nav_avoid_dirt)),

    // SD card. Eject flushes the log and unmounts, so the card can come out
    // without a write in flight. Nothing remounts it until it has been
    // pulled and put back.
    action(0x0C01, "Eject card", "SD card", state::CommandId::StorageEject),
    // The card as a drive on a laptop over the USB-C, parked; again (or
    // unplugging) gives it back (drivers::UsbDrive).
    action(0x0C02, "USB drive",  "SD card", state::CommandId::StorageToHost),

    // Wi-Fi (§11.7): the update hotspot, on or off. DIAG -> Update shows
    // the password and has the same switch.
    action(0x0D01, "Wi-Fi on/off", "Wi-Fi", state::CommandId::WifiToggle),

    // Battery: VIN reads the diode's drop under the battery; this adds it
    // back. Set against a meter on the battery, key on.
    num(0x0E01, "Trim", "Battery", Kind::Float, -1.0f, 1.5f, 0.05f, "V", FIELD(battery_trim_v)),

    // Blackbox. 50 Hz is about 30 MB an hour and gives the ring ten seconds
    // of slack against an SD stall; 5 Hz is a hundred. Lap markers are
    // written whatever this says.
    choice(0x0901, "Rate", "Log", FIELD(log_rate),
           core::log::kRateLabels, core::log::kRateCount),
    // Everything the first rides are for, in detail/ beside the blackbox:
    // raw IMU at 200 Hz, the tach and wheel pulses, the filters' insides,
    // events, the machine's health. About 130 MB an hour.
    boolean(0x0902, "Detail log", "Log", FIELD(detail_log)),

    // Diag: aids for bring-up and tuning, shown over every screen. Last, so
    // the submenus above keep their places (docs/console.md's routes).
    boolean(0x1001, "FPS counter", "Diag", FIELD(show_fps)),
    choice(0x1002, "Frame rate",   "Diag", FIELD(frame_rate), kFrameRateOptions, 6),

    // Service (core::trip::service_due): an alert every Interval engine
    // hours after Done at; 0 is off. Mark done sets Done at to now.
    // The last group, so no older menu route moves.
    num(0x1201, "Interval", "Service", Kind::Float, 0, 200, 1, "h", FIELD(service_every_h)),
    num(0x1202, "Done at",  "Service", Kind::Float, 0, 100000, 1, "h", FIELD(service_at_h)),
    confirmed(0x1203, "Mark done", "Service", state::CommandId::ServiceDone),

    // ECU (core/ecu): listen for the K-line adapter. Off and on does not
    // choose another adapter; the first heard each power-on is it.
    boolean(0x1301, "Adapter", "ECU", FIELD(ecu_on)),
};

#undef FIELD
#undef GEAR
#undef GBASE

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

namespace {
bool same(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// True if item i is the first in the table with its group.
bool opens_group(size_t i) {
    for (size_t j = 0; j < i; ++j) {
        if (same(kItems[j].group, kItems[i].group)) return false;
    }
    return true;
}
}  // namespace

size_t group_count() {
    size_t n = 0;
    for (size_t i = 0; i < kCount; ++i) n += opens_group(i) ? 1 : 0;
    return n;
}

const char* group_name(size_t g) {
    for (size_t i = 0; i < kCount; ++i) {
        if (!opens_group(i)) continue;
        if (g == 0) return kItems[i].group;
        --g;
    }
    return nullptr;
}

size_t group_size(size_t g) {
    const char* name = group_name(g);
    if (!name) return 0;
    size_t n = 0;
    for (size_t i = 0; i < kCount; ++i) n += same(kItems[i].group, name) ? 1 : 0;
    return n;
}

const ConfigItem* group_item(size_t g, size_t idx) {
    const char* name = group_name(g);
    if (!name) return nullptr;
    for (size_t i = 0; i < kCount; ++i) {
        if (!same(kItems[i].group, name)) continue;
        if (idx == 0) return &kItems[i];
        --idx;
    }
    return nullptr;
}

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

float default_value(const ConfigItem& it) { return get(kDefaults, it); }

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

float nudged(const ConfigItem& it, float current, int direction) {
    if (direction == 0 || it.kind == Kind::Action) return clamp(it, current);
    if (it.kind == Kind::Bool) return current >= 0.5f ? 0.0f : 1.0f;
    if (it.kind == Kind::Enum) {
        // Enums wrap: pressing down on the last option is how you get back.
        const int count = it.option_count > 0 ? it.option_count : 1;
        int v = static_cast<int>(current) + (direction > 0 ? 1 : -1);
        if (v < 0) v = count - 1;
        if (v >= count) v = 0;
        return static_cast<float>(v);
    }
    return clamp(it, current + (direction > 0 ? it.step : -it.step));
}

bool nudge(Settings& s, const ConfigItem& it, int direction) {
    if (direction == 0 || it.kind == Kind::Action) return false;
    return set(s, it, nudged(it, get(s, it), direction));
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
                     s.shift_flash_rpm, s.shift_lead_ms};
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
