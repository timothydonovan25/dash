// Custom layouts through the real App: modes from a deck, custom screens
// drawn from it, DIAG and CONFIG pinned, short E's action.
#include "check.hpp"
#include "core/layout/deck.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include <cstring>
#include <vector>

using namespace ui;
using core::nav::Button;
using core::nav::InputEvent;
using core::nav::Mode;
using core::nav::Press;
using core::state::Frame;
namespace lay = core::layout;

namespace {
struct Glass {
    std::vector<Color> px;
    FrameBufferCanvas  canvas;
    Glass() : px(static_cast<size_t>(kScreenW) * kScreenH, 0), canvas(px.data(), kScreenW, kScreenH) {}
    Color at(int x, int y) const { return px[static_cast<size_t>(y) * kScreenW + x]; }
    bool inked(const Rect& r, Color bg) const {
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                if (at(x, y) != bg) return true;
        return false;
    }
};

class Sink final : public ICommandSink {
public:
    bool post(const core::state::Command& c) override { posted.push_back(c); return true; }
    std::vector<core::state::Command> posted;
};

struct Fixture {
    core::config::Settings settings;
    Sink                   sink;
    SystemInfo             sys;
    Context                ctx;
    App                    app;
    Fixture() : ctx{&settings, &sink, &sys}, app(ctx) {
        settings.splash = false;
        settings.gauge_sweep = false;   // the key-on sweep holds toasts back
        app.bind_settings(&settings);
    }
};

void press(App& a, Canvas& c, Button b, Press p = Press::Short) { a.on_input(InputEvent{b, p}, c); }

void frame(App& app, Glass& g, const Frame& f) {
    app.update(f);
    if (app.needs_full_repaint()) {
        app.render(g.canvas, true);
        app.clear_full_repaint();
    }
}

constexpr Color kPanelInk = rgb(30, 60, 90);
constexpr lay::Builtin kSecond = lay::kTrip;

// Stock modes, then ENDURO: a custom screen and a built-in already in CRUISE.
lay::Deck& enduro() {
    static lay::Deck* d = new lay::Deck;
    lay::default_deck(*d);
    lay::Screen& s = d->screens[0];
    std::strcpy(s.name, "ENDURO");
    s.flags = lay::Screen::kTitle;
    s.background = color::kBg;
    s.chrome_n = 1;
    s.readout_n = 1;
    s.e_action = 0x0405;   // CONFIG -> Lap -> Clear laps: asks twice
    d->screen_count = 1;
    lay::Chrome& panel = d->chrome[0];
    panel.shape = lay::Shape::FillRoundRect;
    panel.color = kPanelInk;
    panel.x = 20; panel.y = 60; panel.w = 300; panel.h = 140; panel.radius = 8;
    d->chrome_count = 1;
    lay::Readout& r = d->readouts[0];
    r.field = 1;           // engine.rpm
    r.style = lay::Style::Big;
    r.scale = 6;
    r.ink = color::kText;
    r.ground = kPanelInk;
    r.x = 30; r.y = 70; r.w = 280; r.h = 120;
    d->readout_count = 1;
    lay::ModeDef& m = d->modes[d->mode_count++];
    std::strcpy(m.name, "ENDURO");
    m.count = 2;
    m.screens[0] = lay::kCustomBase;
    m.screens[1] = kSecond;
    return *d;
}
}  // namespace

int main() {
    CASE("the stock deck changes nothing");
    {
        static lay::Deck d;
        lay::default_deck(d);
        Fixture plain, decked;
        Glass g1, g2;
        decked.app.set_deck(&d);
        plain.app.begin(g1.canvas);
        decked.app.begin(g2.canvas);
        CHECK(decked.app.layout().mode_count == plain.app.layout().mode_count);
        for (int m = 0; m < core::nav::kModeCount; ++m)
            CHECK(decked.app.layout().screens[m] == plain.app.layout().screens[m]);
        // Walk every mode with A and every screen with B (DIAG and CONFIG
        // have their own ways in): the same screen, the same title.
        for (int m = 0; m < core::nav::kModeCount; ++m) {
            const bool walk = m < 3;
            const int n = walk ? plain.app.layout().screens[m] : 1;
            for (int i = 0; i < n; ++i) {
                CHECK(plain.app.current_id() == decked.app.current_id());
                CHECK(std::strcmp(plain.app.mode_name(), decked.app.mode_name()) == 0);
                if (walk) {
                    press(plain.app, g1.canvas, Button::B);
                    press(decked.app, g2.canvas, Button::B);
                }
            }
            press(plain.app, g1.canvas, Button::A);
            press(decked.app, g2.canvas, Button::A);
        }
    }

    CASE("a custom mode: its screens, its name, DIAG and CONFIG after it");
    {
        Fixture fx;
        Glass g;
        fx.app.set_deck(&enduro());
        fx.app.begin(g.canvas);
        CHECK(fx.app.layout().mode_count == 6);
        for (int i = 0; i < 3; ++i) press(fx.app, g.canvas, Button::A);
        CHECK(std::strcmp(fx.app.mode_name(), "ENDURO") == 0);
        CHECK(fx.app.current_id() == ScreenId::Custom);
        CHECK(std::strcmp(fx.app.current().name(), "ENDURO") == 0);
        press(fx.app, g.canvas, Button::B);
        CHECK(fx.app.current_id() == static_cast<ScreenId>(kSecond));
        press(fx.app, g.canvas, Button::B);
        CHECK(fx.app.current_id() == ScreenId::Custom);
        press(fx.app, g.canvas, Button::A);
        CHECK(std::strcmp(fx.app.mode_name(), "DIAG") == 0);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
        press(fx.app, g.canvas, Button::E);   // the menu's first page: DIAG still works
        CHECK(fx.app.current_id() == ScreenId::Sensors);
        press(fx.app, g.canvas, Button::A);
        CHECK(std::strcmp(fx.app.mode_name(), "CONFIG") == 0);
        CHECK(fx.app.current_id() == ScreenId::Config);
        press(fx.app, g.canvas, Button::A);
        CHECK(std::strcmp(fx.app.mode_name(), "CRUISE") == 0);
        // Long A is home from anywhere.
        for (int i = 0; i < 3; ++i) press(fx.app, g.canvas, Button::A);
        press(fx.app, g.canvas, Button::A, Press::Long);
        CHECK(fx.app.current_id() == ScreenId::Speed);
    }

    CASE("a custom screen draws its chrome and its readout, and repaints the readout");
    {
        Fixture fx;
        Glass g;
        fx.app.set_deck(&enduro());
        fx.app.resume_place(3 << 8);
        fx.app.begin(g.canvas);
        CHECK(fx.app.current_id() == ScreenId::Custom);
        Frame f{};
        f.engine.rpm = 7000.0f;
        frame(fx.app, g, f);
        CHECK(g.at(22, 120) == kPanelInk);                 // the panel, outside the readout
        CHECK(g.inked(Rect{30, 70, 280, 120}, kPanelInk)); // the digits
        CHECK(g.inked(Rect{0, 0, 480, 28}, color::kBg));   // the title bar

        // A new value: only the readout goes to the panel.
        f.engine.rpm = 9000.0f;
        fx.app.update(f);
        CHECK(!fx.app.needs_full_repaint());
        App::Damage dmg[App::kMaxDamage];
        const size_t n = fx.app.damage(dmg, App::kMaxDamage);
        for (size_t k = 0; k < n; ++k)
            std::printf("    damage %zu: %d,%d %dx%d\n", k, dmg[k].r.x, dmg[k].r.y, dmg[k].r.w, dmg[k].r.h);
        CHECK(n == 1);
        if (n == 1) {
            CHECK(dmg[0].r.x == 30 && dmg[0].r.y == 70 && dmg[0].r.w == 280);
        }
        fx.app.clear_damage();
        fx.app.update(f);
        CHECK(fx.app.damage(dmg, App::kMaxDamage) == 0);   // unchanged: nothing sent
    }

    CASE("short E runs the screen's action, asking twice when CONFIG would");
    {
        Fixture fx;
        Glass g;
        fx.app.set_deck(&enduro());
        fx.app.resume_place(3 << 8);
        fx.app.begin(g.canvas);
        Frame f{};
        frame(fx.app, g, f);
        press(fx.app, g.canvas, Button::E);
        CHECK(fx.sink.posted.empty());
        CHECK(fx.app.current().prompt() != nullptr);
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        press(fx.app, g.canvas, Button::C);                // cancels
        CHECK(fx.app.current().prompt() == nullptr);
        press(fx.app, g.canvas, Button::E);
        press(fx.app, g.canvas, Button::E);
        CHECK(fx.sink.posted.size() == 1);
        if (!fx.sink.posted.empty())
            CHECK(fx.sink.posted[0].id == core::state::CommandId::LapClear);
    }

    CASE("a new deck while running goes home and repaints");
    {
        Fixture fx;
        Glass g;
        fx.app.set_deck(&enduro());
        fx.app.resume_place(3 << 8);
        fx.app.begin(g.canvas);
        fx.app.clear_full_repaint();
        static lay::Deck stock;
        lay::default_deck(stock);
        fx.app.set_deck(&stock, &g.canvas);
        CHECK(fx.app.current_id() == ScreenId::Speed);
        CHECK(fx.app.needs_full_repaint());
        CHECK(fx.app.layout().mode_count == 5);
        fx.app.set_deck(nullptr, &g.canvas);   // back to the compiled-in switch
        CHECK(fx.app.current_id() == ScreenId::Speed);
    }

    CASE("a deck place out of range is ignored");
    {
        Fixture fx;
        Glass g;
        fx.app.set_deck(&enduro());
        fx.app.resume_place((3 << 8) | 7);
        fx.app.begin(g.canvas);
        CHECK(fx.app.current_id() == ScreenId::Speed);
    }

    CASE("every style renders inside its box");
    {
        static lay::Deck d;
        lay::default_deck(d);
        lay::Screen& s = d.screens[0];
        std::strcpy(s.name, "ALL");
        s.flags = 0;
        d.screen_count = 1;
        const lay::Style styles[] = {lay::Style::Value, lay::Style::Big, lay::Style::Text, lay::Style::Bar,
                                     lay::Style::Gauge, lay::Style::Lamp, lay::Style::Spark};
        int i = 0;
        for (lay::Style st : styles) {
            lay::Readout& r = d.readouts[i];
            r.field = 1;
            r.style = st;
            r.ink = color::kAccent;
            r.ground = color::kBg;
            r.lo = 0; r.hi = 12000;
            r.x = static_cast<int16_t>((i % 4) * 120);
            r.y = static_cast<int16_t>((i / 4) * 160);
            r.w = 116; r.h = 150;
            r.scale = 2;
            ++i;
        }
        d.readout_count = static_cast<uint16_t>(i);
        s.readout_n = static_cast<uint8_t>(i);
        d.modes[0].screens[0] = lay::kCustomBase;
        lay::Error e;
        CHECK(lay::validate(d, &e));
        Fixture fx;
        Glass g;
        fx.app.set_deck(&d);
        fx.app.begin(g.canvas);
        Frame f{};
        for (uint32_t t = 0; t < 2000; t += 100) {
            f.t_ms = t;
            f.engine.rpm = static_cast<float>(t * 5);
            frame(fx.app, g, f);
            App::Damage dmg[App::kMaxDamage];
            const size_t n = fx.app.damage(dmg, App::kMaxDamage);
            for (size_t k = 0; k < n; ++k) {
                FrameBufferCanvas win(g.px.data(), kScreenW, kScreenH);
                fx.app.draw_damage(win, dmg[k]);
            }
            fx.app.clear_damage();
        }
        for (int k = 0; k < i; ++k) {
            const lay::Readout& r = d.readouts[k];
            if (!g.inked(Rect{r.x, r.y, r.w, r.h}, color::kBg))
                std::printf("    style %d drew nothing\n", static_cast<int>(r.style));
            CHECK(g.inked(Rect{r.x, r.y, r.w, r.h}, color::kBg));
        }
    }

    CASE("a custom screen is not the largest screen (render's stack)");
    {
        // The slot without it: what render's stack paid before custom screens.
        using Others = ScreenStorage<
            SpeedScreen, MediaScreen, MinimalScreen, TripScreen, NavScreen, TripMapScreen, PhoneScreen,
            RaceScreen, LapScreen, LapHistoryScreen, TrackMapScreen, SessionScreen, AllDataScreen,
            EngineScreen, AttitudeScreen, PerfScreen, StuntScreen, HeartScreen, DiagMenuScreen,
            SensorsScreen, GpsScreen, FusionScreen, SystemScreen, PowerScreen, BusScreen, RadioScreen,
            UpdateScreen, ConfigScreen, GearCalScreen>;
        std::printf("    CustomScreen %zu bytes, the slot without it %zu\n", sizeof(CustomScreen), Others::size);
        CHECK(sizeof(CustomScreen) <= Others::size);
        CHECK(AllScreens::size == Others::size);
    }

    return check::report("custom");
}
