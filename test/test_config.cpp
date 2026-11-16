#include "check.hpp"
#include "core/config/config.hpp"
#include <cstring>

using namespace core::config;

namespace {
bool eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// Every key that has ever shipped. An NVS partition written by an older
// firmware still has these in it, so removing or renumbering one silently
// loses a rider's setting — the odometer lives in the same flash. Adding to
// this list is fine. Changing a number in it is not.
const uint16_t kShippedKeys[] = {
    0x0101, 0x0102, 0x0103, 0x0104, 0x0105, 0x0106, 0x0107, 0x0108, 0x0109, 0x010A, 0x010B,
    0x0201, 0x0202, 0x0203, 0x0204, 0x0205,
    0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x0309, 0x030A,
    0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
    0x0501, 0x0502, 0x0503,
    0x0601, 0x0602, 0x0603,
    0x0701,
    0x0801, 0x0802, 0x0803, 0x0804, 0x0805, 0x0806, 0x0807,
    0x0808, 0x0809, 0x080A,
    0x0811, 0x0812, 0x0813, 0x0814, 0x0815, 0x0816,
    0x0901, 0x0902,
    0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06,
    0x0B01, 0x0B02, 0x0B03,
    0x0C01,
    0x0D01,
    0x0E01,
    0x0F01, 0x0F02,
    0x1001, 0x1002,
    0x1101, 0x1102, 0x0C02,
    0x1201, 0x1202, 0x1203,
    0x1301,
    0x1401, 0x1402, 0x1403, 0x1404,
};
}  // namespace

int main() {
    CASE("the table is internally consistent");
    {
        const char* why = "";
        const bool ok = validate_table(&why);
        if (!ok) std::printf("       table says: %s\n", why);
        CHECK(ok);
        CHECK(item_count() > 0);
    }

    CASE("no shipped NVS key has been renumbered or dropped");
    {
        for (uint16_t k : kShippedKeys) {
            const ConfigItem* it = find(k);
            if (!it) std::printf("       missing key 0x%04X\n", k);
            CHECK(it != nullptr);
        }
        CHECK(item_count() == sizeof(kShippedKeys) / sizeof(kShippedKeys[0]));
        CHECK(find(0xDEAD) == nullptr);      // a key from a newer firmware
    }

    CASE("frame rate: 120 Hz by default, each option its rate, Unlimited 0");
    {
        Settings s;
        CHECK(frame_rate_hz(s) == 120);
        const ConfigItem* it = find(0x1002);
        CHECK(it != nullptr && it->kind == Kind::Enum && it->option_count == kFrameRateCount);
        const uint16_t want[] = {10, 20, 30, 60, 120, 0};
        for (uint8_t i = 0; i < kFrameRateCount; ++i) {
            set(s, *it, i);
            CHECK(frame_rate_hz(s) == want[i]);
        }
        s.frame_rate = 200;                 // a value no menu could set
        CHECK(frame_rate_hz(s) == 120);
    }

    CASE("every item round-trips through its own range");
    {
        for (size_t i = 0; i < item_count(); ++i) {
            const ConfigItem& it = items()[i];
            if (it.kind == Kind::Action) continue;
            Settings s;
            set(s, it, it.min);
            CHECK_NEAR(get(s, it), it.min, 1e-3);
            set(s, it, it.max);
            CHECK_NEAR(get(s, it), it.max, 1e-3);
        }
    }

    CASE("out-of-range values clamp instead of wrapping");
    {
        Settings s;
        const ConfigItem* bright = find(0x0101);       // uint8, 5..100
        set(s, *bright, 5000.0f);
        CHECK_NEAR(get(s, *bright), 100.0, 1e-6);
        CHECK(s.brightness_pct == 100);                // not 5000 & 0xFF
        set(s, *bright, -40.0f);
        CHECK_NEAR(get(s, *bright), 5.0, 1e-6);

        const ConfigItem* tz = find(0x0103);           // int32, -12..14
        set(s, *tz, -99.0f);
        CHECK(s.tz_offset_h == -12);
        set(s, *tz, 99.0f);
        CHECK(s.tz_offset_h == 14);
    }

    CASE("writing one setting never disturbs its neighbours");
    {
        // The failure this catches: an item whose declared range needs two
        // bytes sitting over a one-byte field. The write runs off the end
        // and the setting next door changes on its own.
        Settings s;
        for (size_t i = 0; i < item_count(); ++i) {
            const ConfigItem& it = items()[i];
            if (it.kind != Kind::Action) set(s, it, it.max);
        }
        for (size_t i = 0; i < item_count(); ++i) {
            const ConfigItem& it = items()[i];
            if (it.kind == Kind::Action) continue;
            CHECK_NEAR(get(s, it), it.max, 1e-3);
        }
    }

    CASE("set() reports whether anything actually changed");
    {
        Settings s;
        const ConfigItem* it = find(0x0301);           // first lamp rpm
        CHECK(set(s, *it, 7000.0f));
        CHECK(!set(s, *it, 7000.0f));                  // same value
        CHECK(!set(s, *it, 7000.4f));                  // rounds to the same
        CHECK(set(s, *it, 7100.0f));
    }

    CASE("C and D step by the item's own step size");
    {
        Settings s;
        const ConfigItem* it = find(0x0101);           // brightness, step 5
        s.brightness_pct = 80;
        nudge(s, *it, +1);
        CHECK(s.brightness_pct == 85);
        nudge(s, *it, -1);
        CHECK(s.brightness_pct == 80);
        for (int i = 0; i < 20; ++i) nudge(s, *it, +1);
        CHECK(s.brightness_pct == 100);                // stops at the top
    }

    CASE("enums wrap so a rider can reach every option");
    {
        Settings s;
        const ConfigItem* it = find(0x0102);           // units
        CHECK(s.unit_system == 0);
        nudge(s, *it, +1);
        CHECK(s.unit_system == 1);
        nudge(s, *it, +1);
        CHECK(s.unit_system == 0);                     // wrapped
        nudge(s, *it, -1);
        CHECK(s.unit_system == 1);
    }

    CASE("bools toggle");
    {
        Settings s;
        const ConfigItem* it = find(0x0104);
        CHECK(!s.dim_at_night);
        nudge(s, *it, +1);
        CHECK(s.dim_at_night);
        nudge(s, *it, +1);
        CHECK(!s.dim_at_night);
    }

    CASE("actions carry a command and hold no value");
    {
        Settings s;
        const ConfigItem* it = find(0x0404);
        CHECK(it->kind == Kind::Action);
        CHECK(it->action == core::state::CommandId::LapSetLine);
        CHECK(!set(s, *it, 1.0f));
        CHECK(!nudge(s, *it, +1));
    }

    CASE("the config screen draws each row from the table alone");
    {
        Settings s;
        char buf[32];
        format(s, *find(0x0101), buf, sizeof buf);  CHECK(eq(buf, "80 %"));
        format(s, *find(0x0102), buf, sizeof buf);  CHECK(eq(buf, "Imperial"));
        format(s, *find(0x0104), buf, sizeof buf);  CHECK(eq(buf, "Off"));
        format(s, *find(0x0301), buf, sizeof buf);  CHECK(eq(buf, "6000 rpm"));
        format(s, *find(0x0501), buf, sizeof buf);  CHECK(eq(buf, "0.18 gph"));
        format(s, *find(0x0404), buf, sizeof buf);  CHECK(eq(buf, "press E"));
        format(s, *find(0x0901), buf, sizeof buf);  CHECK(eq(buf, "10 Hz"));
    }

    CASE("the log rate is an enum of known rates, and it wraps");
    {
        Settings s;
        const ConfigItem* it = find(0x0901);
        CHECK(it != nullptr);
        CHECK(it->kind == Kind::Enum);
        CHECK(it->option_count == core::log::kRateCount);

        // Down from the default walks the list; down off the end comes back
        // round to the fastest rate, so Off is one press from 1 Hz.
        CHECK(nudge(s, *it, -1));
        CHECK(s.log_rate == static_cast<uint8_t>(core::log::Rate::Hz5));
        CHECK(nudge(s, *it, -1));
        CHECK(nudge(s, *it, -1));
        CHECK(s.log_rate == static_cast<uint8_t>(core::log::Rate::Off));
        CHECK(nudge(s, *it, -1));
        CHECK(s.log_rate == static_cast<uint8_t>(core::log::Rate::Hz50));

        // What the menu shows is the rate the decimator will keep.
        char buf[32];
        format(s, *it, buf, sizeof buf);
        CHECK(eq(buf, "50 Hz"));
        CHECK(core::log::rate_interval_ms(s.log_rate) == 20);
    }

    CASE("the sections cover every item exactly once, a group's items together");
    {
        size_t total = 0;
        CHECK(section_count() >= 6 && section_count() <= 9);   // a short top level
        CHECK(eq(section_name(0), "Display"));
        CHECK(section_name(section_count()) == nullptr);
        for (size_t s = 0; s < section_count(); ++s) {
            CHECK(section_size(s) > 0);
            CHECK(section_groups(s) >= 1);
            size_t changes = 0;
            for (size_t i = 0; i < section_size(s); ++i) {
                const ConfigItem* it = section_item(s, i);
                CHECK(it != nullptr);
                if (i > 0 && !eq(it->group, section_item(s, i - 1)->group)) ++changes;
            }
            // Each group is one run, so a heading appears once per group.
            CHECK(changes + 1 == section_groups(s));
            CHECK(section_item(s, section_size(s)) == nullptr);
            total += section_size(s);
        }
        CHECK(total == item_count());
        const char* why = nullptr;
        CHECK(validate_table(&why));
    }

    CASE("Display ends with Startup: the sweep (on, 1 s) and the splash (on)");
    {
        Settings s;
        CHECK(s.gauge_sweep);
        CHECK(s.splash);
        CHECK(s.sweep_ms == 1000);
        const size_t n = section_size(0);
        CHECK(eq(section_item(0, n - 3)->group, "Startup"));
        CHECK(section_item(0, n - 3)->key == 0x0B01);
        CHECK(section_item(0, n - 1)->key == 0x0B03);
        char buf[32];
        format(s, *find(0x0B02), buf, sizeof buf);  CHECK(eq(buf, "1000 ms"));
    }

    CASE("the modules that keep their own config read it from here");
    {
        Settings s;
        s.shift_base_rpm = 7000;
        s.lap_min_lap_s  = 25;
        const ShiftView sv = shift_view(s);
        CHECK(sv.base_rpm == 7000);
        CHECK(sv.flash_rpm == s.shift_flash_rpm);
        CHECK(sv.lead_ms == 0);                  // off until tried on the bike
        s.shift_lead_ms = 120;
        CHECK(shift_view(s).lead_ms == 120);
        const LapView lv = lap_view(s);
        CHECK(lv.min_lap_ms == 25000);
        CHECK_NEAR(lv.half_width_ft, s.lap_half_width_ft, 1e-6);
    }

    CASE("default_value is a fresh Settings' value, whatever the current one is");
    {
        Settings s;
        s.brightness_pct = 40;
        s.slip_bar = false;
        CHECK(default_value(*find(0x0101)) == 80.0f);
        CHECK(default_value(*find(0x0106)) == 1.0f);
        CHECK(default_value(*find(0x0405)) == 0.0f);   // an action
    }

    CASE("only the actions that lose something ask twice");
    {
        CHECK(find(0x0405)->confirm);   // Clear laps
        CHECK(find(0x0406)->confirm);   // Forget track
        CHECK(find(0x0A01)->confirm);   // Forget phone
        CHECK(!find(0x0C01)->confirm);  // Eject card
        CHECK(!find(0x030A)->confirm);  // Test lamps
        CHECK(!find(0x0101)->confirm);  // not an action
    }

    CASE("nudged gives the next value without touching Settings");
    {
        using namespace core::config;
        const ConfigItem* bright = find(0x0101);
        CHECK(bright != nullptr);
        CHECK_NEAR(nudged(*bright, 80.0f, +1), 80.0f + bright->step, 1e-6);
        CHECK_NEAR(nudged(*bright, bright->max, +1), bright->max, 1e-6);   // clamped
        CHECK_NEAR(nudged(*bright, bright->min, -1), bright->min, 1e-6);
        // nudge is nudged applied, so the two cannot disagree.
        Settings s;
        const float from = get(s, *bright);
        CHECK(nudge(s, *bright, -1));
        CHECK_NEAR(get(s, *bright), nudged(*bright, from, -1), 1e-6);
    }

    CASE("the mount offset takes a calibration unrounded, pitch to 89 degrees");
    {
        using namespace core::config;
        Settings s;
        const ConfigItem* pitch = find(kKeyImuPitchOffset);
        const ConfigItem* roll = find(kKeyImuRollOffset);
        CHECK(pitch != nullptr && roll != nullptr);
        CHECK(set(s, *pitch, 52.37f));
        CHECK_NEAR(s.imu_pitch_offset_deg, 52.37, 1e-5);
        set(s, *pitch, 120.0f);
        CHECK_NEAR(s.imu_pitch_offset_deg, 89.0, 1e-5);
        CHECK(set(s, *roll, -3.21f));
        CHECK_NEAR(s.imu_roll_offset_deg, -3.21, 1e-5);
    }

    return check::report("config");
}
