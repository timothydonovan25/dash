// core::layout: the field table, the stock deck, validation, the .dui format.
#include "check.hpp"
#include "core/layout/deck.hpp"
#include "core/layout/fields.hpp"
#include "core/state/slots.hpp"
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace core::layout;

namespace {
// Big: on the heap, not the stack.
Deck& fresh() {
    static Deck* d = new Deck;
    default_deck(*d);
    return *d;
}

// One custom screen in its own mode, with a panel and a value inside it.
Deck& with_custom() {
    Deck& d = fresh();
    Screen& s = d.screens[0];
    std::strcpy(s.name, "ENDURO");
    s.flags = Screen::kTitle;
    s.chrome_first = 0; s.chrome_n = 2;
    s.readout_first = 0; s.readout_n = 2;
    d.screen_count = 1;

    Chrome& panel = d.chrome[0];
    panel.shape = Shape::FillRoundRect;
    panel.x = 10; panel.y = 40; panel.w = 200; panel.h = 100; panel.radius = 6;
    Chrome& label = d.chrome[1];
    label.shape = Shape::Text;
    std::strcpy(label.text, "HELLO");
    label.x = 240; label.y = 40; label.scale = 2;
    d.chrome_count = 2;

    Readout& a = d.readouts[0];
    a.field = 1;   // engine.rpm
    a.x = 20; a.y = 50; a.w = 180; a.h = 80;
    Readout& b = d.readouts[1];
    b.field = 1;
    b.style = Style::Bar;
    b.x = 10; b.y = 200; b.w = 460; b.h = 20; b.lo = 0; b.hi = 12000;
    d.readout_count = 2;

    ModeDef& m = d.modes[d.mode_count++];
    std::strcpy(m.name, "ENDURO");
    m.count = 2;
    m.screens[0] = kCustomBase + 0;
    m.screens[1] = kSpeed;   // a built-in may appear in two modes
    return d;
}

std::vector<uint8_t> enc(const Deck& d) {
    std::vector<uint8_t> b(kMaxEncoded);
    b.resize(encode(d, b.data(), b.size()));
    return b;
}

bool refused(const Deck& d, const char* what) {
    Error e;
    const bool ok = validate(d, &e);
    if (ok) std::printf("    accepted, wanted \"%s\"\n", what);
    else if (std::strcmp(e.what, what) != 0) std::printf("    got \"%s\", wanted \"%s\"\n", e.what, what);
    return !ok && std::strcmp(e.what, what) == 0;
}
}  // namespace

int main() {
    CASE("the field table: every id once, sorted, every getter runs");
    {
        std::set<uint16_t> ids;
        std::set<std::string> paths;
        core::state::Frame f{};
        uint16_t prev = 0;
        for (size_t i = 0; i < field_count(); ++i) {
            const FieldDef& d = fields()[i];
            CHECK(d.id > prev);
            prev = d.id;
            CHECK(ids.insert(d.id).second);
            CHECK(paths.insert(d.path).second);
            CHECK(d.caption && d.caption[0]);
            (void)d.get(f);
            CHECK(find_field(d.id) == &d);
        }
        CHECK(field_count() > 400);
        CHECK(find_field(0) == nullptr);
        CHECK(find_field(60000) == nullptr);
    }

    CASE("fields read the Frame, with units and health");
    {
        core::state::Frame f{};
        const FieldDef* rpm = nullptr;
        const FieldDef* psi = nullptr;
        const FieldDef* title = nullptr;
        const FieldDef* lean = nullptr;
        const FieldDef* gnss_state = nullptr;
        for (size_t i = 0; i < field_count(); ++i) {
            const FieldDef& d = fields()[i];
            const std::string p = d.path;
            if (p == "engine.rpm") rpm = &d;
            if (p == "tpms.front.psi") psi = &d;
            if (p == "media.title") title = &d;
            if (p == "att.lean_deg") lean = &d;
            if (p == "health.dev[Gnss].state") gnss_state = &d;
        }
        CHECK(rpm && psi && title && lean && gnss_state);
        CHECK(rpm->id == 1);   // the ids file is stable: engine.rpm was first
        f.engine.rpm = 8234.0f;
        ShowCtx c;
        Shown s = show(*rpm, f, c);
        CHECK(std::string(s.text) == "8230");   // core::units: rpm to the nearest 10
        CHECK(s.valid);

        f.tpms.front.psi = 14.5f;
        s = show(*psi, f, c);
        CHECK(!s.valid);                         // the TPMS sensor is not working
        f.health[core::state::Dev::TpmsF].state = core::state::DevState::Ok;
        s = show(*psi, f, c);
        CHECK(s.valid);
        CHECK(std::string(s.text) == "14.5");
        CHECK(std::string(s.unit) == "psi");
        c.units = core::units::System::Metric;
        s = show(*psi, f, c);
        CHECK(std::string(s.unit) == "bar");
        CHECK_NEAR(s.number, 14.5, 1e-6);        // ranges stay native

        std::strcpy(f.media.title, "Flying Whales");
        s = show(*title, f, c);
        CHECK(std::string(s.text) == "Flying Whales");

        f.health[core::state::Dev::Gnss].state = core::state::DevState::Degraded;
        s = show(*gnss_state, f, c);
        CHECK(std::string(s.text) == "Degraded");

        s = show(*lean, f, c);
        CHECK(std::string(s.text) == "--");      // the IMU is absent
    }

    CASE("the stock deck is today's layout and valid");
    {
        Deck& d = fresh();
        CHECK(d.mode_count == 3);
        CHECK(d.modes[0].count == 7 && d.modes[1].count == 5 && d.modes[2].count == 6);
        CHECK(d.modes[0].screens[0] == kSpeed);
        CHECK(std::string(d.modes[1].name) == "TRACK");
        Error e;
        CHECK(validate(d, &e));
    }

    CASE("round trip through the .dui format");
    {
        Deck& d = with_custom();
        Error e;
        CHECK(validate(d, &e));
        std::vector<uint8_t> b = enc(d);
        CHECK(b.size() > 20);
        static Deck back;
        CHECK(decode(b.data(), b.size(), back, &e));
        CHECK(back.mode_count == 4);
        CHECK(back.screen_count == 1);
        CHECK(std::string(back.screens[0].name) == "ENDURO");
        CHECK(back.chrome[1].shape == Shape::Text);
        CHECK(std::string(back.chrome[1].text) == "HELLO");
        CHECK(back.readouts[1].hi == 12000.0f);
        CHECK(enc(back) == b);

        // Damage, truncation, garbage.
        std::vector<uint8_t> bad = b;
        bad[10] ^= 1;
        CHECK(!decode(bad.data(), bad.size(), back, &e));
        CHECK(std::string(e.what) == "damaged (CRC)");
        CHECK(!decode(b.data(), b.size() - 5, back, &e));
        const uint8_t junk[] = {'h', 'e', 'l', 'l', 'o', 0, 0, 0, 0};
        CHECK(!decode(junk, sizeof junk, back, &e));
        CHECK(!decode(b.data(), 3, back, &e));
        CHECK(encode(d, b.data(), 10) == 0);   // too small a buffer
    }

    CASE("validation refuses what would break on the glass");
    {
        Deck& d = with_custom();
        d.mode_count = 0;
        CHECK(refused(d, "no riding mode"));

        Deck& d2 = with_custom();
        d2.modes[3].screens[1] = kConfig;
        CHECK(refused(d2, "screen not allowed in a mode"));

        Deck& d3 = with_custom();
        d3.modes[3].screens[0] = kCustomBase + 5;
        CHECK(refused(d3, "screen not allowed in a mode"));

        Deck& d4 = with_custom();
        d4.modes[0].count = 0;
        CHECK(refused(d4, "mode with no screens"));

        Deck& d5 = with_custom();
        d5.readouts[1].y = 60;   // onto the first readout
        d5.readouts[1].x = 30;
        d5.readouts[1].w = 100;
        CHECK(refused(d5, "two readouts overlap"));

        Deck& d6 = with_custom();
        d6.readouts[0].y = 10;   // under the title bar
        CHECK(refused(d6, "readout under the title"));
        d6.screens[0].flags = 0; // no title: allowed, but now it leaves its panel
        CHECK(refused(d6, "a shape crosses a readout"));

        Deck& d7 = with_custom();
        d7.readouts[1].x = 300;  // off the right edge
        CHECK(refused(d7, "readout off the glass"));

        Deck& d8 = with_custom();
        d8.chrome[1].y = 205;    // the text over the bar
        d8.chrome[1].x = 20;
        CHECK(refused(d8, "a shape crosses a readout"));

        Deck& d9 = with_custom();
        d9.readouts[1].hi = 0;
        CHECK(refused(d9, "range: hi must be above lo"));

        Deck& d10 = with_custom();
        std::memset(d10.modes[0].name, 'A', kNameLen);   // no NUL
        CHECK(refused(d10, "mode name"));

        Deck& d11 = with_custom();
        d11.chrome[0].shape = Shape::Icon;
        d11.chrome[0].icon = 99;
        CHECK(refused(d11, "unknown icon"));

        Deck& d12 = with_custom();
        d12.readouts[0].style = Style::Spark;
        d12.readouts[1].style = Style::Spark;
        CHECK(validate(d12));
        d12.readouts[d12.readout_count] = d12.readouts[1];
        d12.readouts[d12.readout_count].y = 240;
        d12.readout_count++;
        d12.screens[0].readout_n = 3;
        CHECK(refused(d12, "too many traces on a screen"));

        Deck& d13 = with_custom();
        d13.screens[0].e_action = 0x0101;   // brightness: a setting, not an action
        CHECK(refused(d13, "E is not an action"));

        Deck& d14 = with_custom();
        d14.readouts[0].field = 60000;      // a field this firmware does not have
        CHECK(validate(d14));
    }

    CASE("an outline panel around a readout is fine, a line through it is not");
    {
        Deck& d = with_custom();
        d.chrome[0].shape = Shape::RoundRect;
        CHECK(validate(d));
        d.chrome[0].shape = Shape::Line;
        CHECK(refused(d, "a shape crosses a readout"));
    }

    return check::report("layout");
}
