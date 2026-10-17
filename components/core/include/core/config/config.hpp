// One table drives the whole CONFIG mode (§13).
//
// The screen renders it, the NVS load and save walk it, the diag dump
// prints it, and adding a setting is one line in one array. No per-screen
// field lists exist anywhere in this firmware.
//
// Two deliberate differences from the sketch in the architecture document:
//
//  - An item stores a byte OFFSET into `Settings`, not a `void*` target.
//    A pointer table would have to point at a single mutable instance, and
//    that instance would be a global — rule 1. An offset lets `Settings`
//    be an ordinary object that main owns and passes by reference.
//  - An `Action` item carries the CommandId it posts, so the config screen
//    stays a renderer of this table and does not grow a switch statement.
//
// `key` is the NVS key. It is stable forever. Never renumber one, never
// reuse a retired one: that is what lets an old NVS partition still load
// after a firmware change.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/state/command.hpp"
#include "core/gear/gear.hpp"
#include "core/log/log.hpp"
#include "core/units/units.hpp"

namespace core::config {

enum class Kind : uint8_t { Float = 0, Int, Bool, Enum, Action };

// Every tunable in the firmware. Owned by main, passed by reference to the
// services that need it. Trivially copyable so a task can take a snapshot.
struct Settings {
    // Display
    uint8_t brightness_pct = 80;
    uint8_t unit_system    = static_cast<uint8_t>(units::System::Imperial);
    int32_t tz_offset_h    = -5;
    bool    dim_at_night   = false;
    // The wheelspin / lockup bar across the top of every screen (core/slip).
    bool    slip_bar       = true;

    // The start-up gauge sweep (core/sweep): on or off, and how long the
    // needles take to go up and come back.
    bool     gauge_sweep    = true;
    uint16_t sweep_ms       = 2000;
    // The name card before the sweep: CRF250F, the firmware, a bar.
    bool     splash         = true;

    // What the big speed number shows: 0 the wheels (front, then rear, then
    // GPS), 1 the fused estimate (core/fusion). Wheels until the filter has
    // been tuned on real rides.
    uint8_t speedo_source  = 0;

    // Wheels — the front sensor counts spokes, the rear counts sprocket bolts
    uint16_t front_circumference_mm = 2130;
    uint16_t rear_circumference_mm  = 2050;
    uint8_t  front_pulses_per_rev   = 6;
    uint8_t  rear_pulses_per_rev    = 4;

    // Gearing — rpm per mph in each gear. These depend on the sprockets and
    // the rolling radius, so they are calibration rather than constants:
    // ride at a steady speed in each gear and read the ratio off DIAG.
    uint8_t gear_count = 5;
    float   gear_rpm_per_mph[core::gear::kMaxGears] =
        {420.0f, 290.0f, 215.0f, 165.0f, 130.0f, 0.0f};
    // The table as the guided run last measured it, or as it was last typed
    // in. The learner (core/gearcal) may never move an entry more than 5 %
    // from this, so it cannot walk away from a good calibration.
    float   gear_base_rpm_per_mph[core::gear::kMaxGears] =
        {420.0f, 290.0f, 215.0f, 165.0f, 130.0f, 0.0f};
    // Let the learner nudge the table, 1 % at a time, after a lot of agreeing
    // evidence. The guided run does not depend on it.
    bool    gear_autotune = true;

    // Shift lights — mirrors core::shift::Config
    uint16_t shift_base_rpm  = 6000;
    uint16_t shift_span_rpm  = 2400;
    uint16_t shift_hyst_rpm  = 120;
    uint16_t shift_flash_rpm = 8800;
    // The lamps' brightness. On USB it is also capped (core::power).
    uint8_t  shift_brightness_pct = 100;
    // Engine revolutions per tach pulse: 1 if the ignition sparks every
    // revolution, 2 if only on the compression stroke. BRINGUP: compare the
    // dash with a known idle; reading half is 1 where it should be 2.
    uint8_t  tach_revs_per_pulse = 1;
    // Predictive lamps (core::shift::Config::lead_ms): light them this far
    // ahead of the rpm while it climbs. 0, off, until it has been tried on
    // the bike: the right number is about a reaction time, and nobody's is
    // known yet.
    uint16_t shift_lead_ms = 0;

    // Lap timer — mirrors core::lap::Config
    float    lap_half_width_ft = 50.0f;
    float    lap_min_speed_mph = 10.0f;
    uint16_t lap_min_lap_s     = 10;

    // Blackbox — how often `logic` puts a row in the ring. One of
    // core::log::Rate, not a Hz number; see the note on that enum.
    uint8_t log_rate = static_cast<uint8_t>(core::log::kDefaultRate);

    // Fuel model
    float fuel_idle_gph = 0.18f;
    float fuel_wot_gph  = 1.40f;
    float fuel_tank_gal = 1.60f;

    // IMU mount offset, written by the ImuCalibrate command rather than
    // typed in, but shown here so the diag dump includes it.
    float imu_roll_offset_deg  = 0.0f;
    float imu_pitch_offset_deg = 0.0f;
};

struct ConfigItem {
    uint16_t          key;
    const char*       label;
    const char*       group;
    Kind              kind;
    float             min;
    float             max;
    float             step;
    const char*       unit;
    uint16_t          offset;        // byte offset into Settings
    // Width of the field in bytes, taken from the member itself rather than
    // guessed from the range. An item whose declared range implied a wider
    // type than the field really is would write over the setting next door,
    // which is the kind of bug that shows up as an unrelated value changing.
    uint8_t           width;
    const char* const* options;      // Enum only
    uint8_t           option_count;  // Enum only
    state::CommandId  action;        // Action only
};

// The table, and how much of it there is.
const ConfigItem* items();
size_t            item_count();

// The submenus CONFIG opens on: one per distinct `group`, in the order the
// group first appears in the table. Item `i` of group `g` is the i-th item
// of that group in table order. Nullptr / 0 past the end.
size_t            group_count();
const char*       group_name(size_t g);
size_t            group_size(size_t g);
const ConfigItem* group_item(size_t g, size_t i);

// Nullptr if no item owns that key — which is what loading an NVS blob
// written by a newer firmware looks like, and is not an error.
const ConfigItem* find(uint16_t key);

// Everything is carried as a float across this interface, including bools
// and enums. One accessor means the screen, the NVS code and the logger all
// go through the same clamp.
float clamp(const ConfigItem& it, float v);
float get(const Settings& s, const ConfigItem& it);
bool  set(Settings& s, const ConfigItem& it, float v);   // true if it changed

// One step of the C/D buttons on the config screen.
bool nudge(Settings& s, const ConfigItem& it, int direction);

// The value as the config screen draws it: "80 %", "Metric", "On".
size_t format(const Settings& s, const ConfigItem& it, char* buf, size_t n);

// Copy the tunables that other modules keep their own config structs for.
// Called after any ConfigSet so nothing goes on using a stale threshold.
struct ShiftView { uint16_t base_rpm, span_rpm, hysteresis_rpm, flash_rpm, lead_ms; };
struct LapView   { double half_width_ft; float min_speed_mph; uint32_t min_lap_ms; };
ShiftView shift_view(const Settings& s);
LapView   lap_view(const Settings& s);
core::gear::Config gear_view(const Settings& s);

// Checks the table against itself: no duplicate keys, no key 0, offsets and
// ranges sane, enums with options, actions without offsets. The host test
// calls this; so does the DIAG screen on a debug build, because a bad table
// is otherwise only discovered by a setting that will not save.
bool validate_table(const char** why);

}  // namespace core::config
