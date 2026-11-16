// Every datapoint in the Frame, for custom screens (docs/custom-screens.md §3).
//
// The table is generated from the Frame's headers by tools/frame_fields.py,
// so nothing in Frame can be left out of it. A custom screen's readout names
// a field by its id, which is stable forever (components/core/layout_fields.ids).
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/units/units.hpp"

namespace core::state { struct Frame; }

namespace core::layout {

// One field's value, read from a Frame. Strings point into that Frame.
struct Value {
    enum Kind : uint8_t { Num = 0, Bool, Str, Enum } kind = Num;
    double      num = 0.0;
    const char* str = "";
};

enum class Type : uint8_t { Num = 0, Int, Bool, Str, Enum };

// How a number is shown. Speed, distance, temperature, pressure and volume
// follow CONFIG -> Units through core::units; the rest show `unit` as is.
enum class Quantity : uint8_t {
    Plain = 0, Speed, DistMi, DistFt, AltM, Temp, Pressure, Volume,
    Duration,   // milliseconds as m:ss.t
    Seconds,    // whole seconds as h:mm:ss
    Clock,      // UTC milliseconds of the day, as the rider's time of day
    Rpm,
};

struct EnumName { int value; const char* name; };
struct EnumNames { const EnumName* items; size_t count; };

struct FieldDef {
    uint16_t         id;
    const char*      path;      // "engine.rpm", "tpms.front.psi"
    const char*      caption;   // what a readout says by default
    const char*      unit;      // the native unit, before CONFIG -> Units
    const char*      column;    // the blackbox column it matches, "" for none
    Type             type;
    Quantity         q;
    uint8_t          decimals;
    int8_t           dev;       // core::state::Dev that must be working, -1 none
    const EnumNames* names;     // Enum only
    Value          (*get)(const core::state::Frame&);
};

const FieldDef* fields();
size_t          field_count();
// Null for an id this firmware does not have (retired, or from a newer one).
const FieldDef* find_field(uint16_t id);
// The enum's name for `v`, or null.
const char*     enum_name(const FieldDef& f, int v);

// What a readout shows, after units. `text` is the value as the rider reads
// it ("63", "1:42.3", "ENDURO", "--"); `unit` its unit after CONFIG -> Units
// ("km/h"); `number` the same value as a plain number for bars and gauges,
// in the field's NATIVE unit so a range set in the editor never changes
// meaning with the units setting; `valid` false draws it dim.
struct Shown {
    char        text[32] = {};
    const char* unit = "";
    double      number = 0.0;
    bool        valid = false;
};

struct ShowCtx {
    units::System units = units::System::Imperial;
    int           tz_offset_h = 0;
    bool          clock_12h = true;
};

// `decimals` < 0 takes the field's own.
Shown show(const FieldDef& f, const core::state::Frame& fr, const ShowCtx& c,
           int decimals = -1);

}  // namespace core::layout
