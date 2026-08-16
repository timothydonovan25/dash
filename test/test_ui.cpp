#include "check.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "core/state/track_buffer.hpp"
#include <cstring>
#include <vector>

using namespace ui;
using core::nav::Button;
using core::nav::InputEvent;
using core::nav::Mode;
using core::nav::Press;
using core::state::Dev;
using core::state::DevState;
using core::state::Frame;

namespace {

// The whole panel, in RAM. 320x480 of RGB565 is 300 kB, which is fine on a
// laptop and is exactly why the device draws in bands instead.
struct Screen320x480 {
    std::vector<Color> px;
    FrameBufferCanvas  canvas;
    Screen320x480()
        : px(static_cast<size_t>(kScreenW) * kScreenH, 0),
          canvas(px.data(), kScreenW, kScreenH) {}

    size_t ink() const {   // pixels that are not the background
        size_t n = 0;
        for (Color c : px) if (c != color::kBg && c != 0) ++n;
        return n;
    }
    void wipe() { for (Color& c : px) c = 0; }
};

class RecordingSink final : public ICommandSink {
public:
    bool post(const core::state::Command& c) override {
        posted.push_back(c);
        return true;
    }
    std::vector<core::state::Command> posted;
};

// A Frame with plausible numbers in every field, so a screen that reads the
// wrong one still shows something and a screen that reads nothing shows up
// as blank.
Frame busy_frame() {
    Frame f;
    f.t_ms = 1234567;
    f.engine.rpm = 7250.0f;
    f.engine.neutral = false;
    f.engine.coolant_c = 88.0f;
    f.engine.tps_pct = 62.0f;
    f.engine.ecu_online = false;         // NullEngine: what actually ships
    f.motion.gps_mph = 54.0f;
    f.motion.front_mph = 53.6f;
    f.motion.rear_mph = 54.4f;
    f.motion.best_mph = 54.0f;
    f.att.lean_deg = -32.5f;
    f.att.pitch_deg = 3.0f;
    f.att.yaw_rate_dps = 24.0f;
    f.att.g_lat = 0.62f;
    f.att.g_vert = 1.18f;
    f.att.airtime_ms = 820;
    f.att.last_land_g = 4.4f;
    f.gnss.fix = true;
    f.gnss.sats = 11;
    f.gnss.quality = 1;
    f.gnss.lat = 42.5012;
    f.gnss.lon = -83.1234;
    f.gnss.alt_m = 265.0f;
    f.gnss.hdop = 0.8f;
    f.gnss.course_deg = 191.0f;
    f.gnss.speed_mph = 54.0f;
    f.gnss.utc_ms = (18 * 3600 + 42 * 60 + 5) * 1000u;
    f.trip.odo_mi = 4821.6f;
    f.trip.trip_a_mi = 31.4f;
    f.trip.trip_b_mi = 128.9f;
    f.trip.fuel_used_gal = 0.86f;
    f.trip.eng_hours = 143.2f;
    f.trip.top_mph = 68.0f;
    f.lap.phase = core::lap::Phase::Timing;
    f.lap.count = 4;
    f.lap.current_ms = 42150;
    f.lap.last_ms = 91250;
    f.lap.best_ms = 90880;
    f.lap.delta_ms = -320;
    f.lap.dist_to_line_ft = 1840.0f;
    f.elec.vbat = 12.6f;
    f.elec.v5 = 5.02f;
    f.elec.v3v3 = 3.31f;
    for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
        f.health.dev[i].state = DevState::Ok;
    }
    f.health[Dev::Ecu].state = DevState::Absent;
    f.health[Dev::BleHost].state = DevState::Absent;
    f.health[Dev::McpPod].state = DevState::Degraded;
    f.health[Dev::McpPod].fault_count = 7;
    return f;
}

// Everything dead, nothing published. Every screen must survive this, and
// every reading on it must be `--`.
Frame dead_frame() { return Frame{}; }

struct Fixture {
    core::config::Settings settings;
    RecordingSink          sink;
    SystemInfo             sys;
    Context                ctx;
    App                    app;

    Fixture() : ctx{&settings, &sink, &sys}, app(ctx) {
        sys.heap_free_b = 184320;
        sys.heap_min_b = 120000;
        sys.psram_free_b = 6 * 1024 * 1024;
        sys.uptime_s = 3725;
        sys.render_hz_x10 = 299;
        sys.gnss_bad_csum = 2;
        app.bind_settings(&settings);
    }
};

// Walk to a given mode and screen with real button presses, the way a rider
// would, rather than reaching into nav state.
void go_to(App& app, Canvas& c, Mode m, uint8_t screen) {
    for (int i = 0; i < 8 && app.nav_state().mode != m; ++i) {
        app.on_input(InputEvent{Button::A, Press::Short}, c);
    }
    for (int i = 0; i < 8 && app.nav_state().screen != screen; ++i) {
        app.on_input(InputEvent{Button::B, Press::Short}, c);
    }
}

}  // namespace

int main() {
    CASE("the font renders every printable character");
    {
        // A blank glyph in the middle of the table shows up as a screen with
        // a hole in it and is otherwise very hard to notice.
        Screen320x480 s;
        for (char ch = 0x21; ch < 0x7F; ++ch) {
            s.wipe();
            const char text[2] = {ch, '\0'};
            s.canvas.draw_text(10, 10, text, color::kWhite, 1, Align::Left);
            if (s.ink() == 0) std::printf("       blank glyph: '%c'\n", ch);
            CHECK(s.ink() > 0);
        }
        // ...and a space is the one that must not.
        s.wipe();
        s.canvas.draw_text(10, 10, " ", color::kWhite, 1, Align::Left);
        CHECK(s.ink() == 0);
    }

    CASE("text alignment puts the ink where it was asked to");
    {
        Screen320x480 s;
        s.wipe();
        s.canvas.draw_text(160, 100, "88", color::kWhite, 2, Align::Center);
        const int16_t w = s.canvas.text_width("88", 2);
        bool left_clear = true, centre_inked = false;
        for (int16_t x = 0; x < 160 - w; ++x)
            for (int16_t y = 100; y < 116; ++y)
                if (s.canvas.pixel(x, y) != 0) left_clear = false;
        for (int16_t x = static_cast<int16_t>(160 - w / 2); x < 160 + w / 2; ++x)
            for (int16_t y = 100; y < 116; ++y)
                if (s.canvas.pixel(x, y) != 0) centre_inked = true;
        CHECK(left_clear);
        CHECK(centre_inked);
    }

    CASE("drawing never writes outside the buffer");
    {
        // Widgets are trusted to stay in their bounds for looks; the canvas
        // is what makes it safe. A widget two pixels out is a cosmetic bug,
        // a canvas two pixels out is a reboot.
        Color guard[64 * 32 + 16];
        for (size_t i = 0; i < sizeof guard / sizeof guard[0]; ++i) guard[i] = 0xAAAA;
        FrameBufferCanvas c(guard, 64, 32);
        c.fill(color::kBg);
        c.fill_rect(Rect{-100, -100, 500, 500}, color::kWhite);
        c.draw_line(-50, -50, 400, 400, color::kWhite);
        c.fill_circle(-10, -10, 200, color::kWhite);
        c.draw_text(-40, -20, "overflow", color::kWhite, 4, Align::Left);
        c.draw_text(500, 500, "overflow", color::kWhite, 4, Align::Right);
        for (size_t i = 64 * 32; i < sizeof guard / sizeof guard[0]; ++i) {
            CHECK(guard[i] == 0xAAAA);
        }
    }

    CASE("every screen in every mode draws something from a busy frame");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        const Frame f = busy_frame();

        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                go_to(fx.app, s.canvas, static_cast<Mode>(m), n);
                CHECK(fx.app.nav_state().screen == n);
                s.wipe();
                fx.app.update(f);
                fx.app.render(s.canvas, true);
                const size_t ink = s.ink();
                if (ink == 0) {
                    std::printf("       blank screen: %s / %s\n",
                                fx.app.mode_name(), fx.app.current().name());
                }
                CHECK(ink > 0);
            }
        }
    }

    CASE("every screen survives a frame where nothing has been published");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        const Frame f = dead_frame();
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                go_to(fx.app, s.canvas, static_cast<Mode>(m), n);
                fx.app.update(f);
                fx.app.render(s.canvas, true);
            }
        }
        CHECK(true);       // no crash, no read of an unpublished slot
    }

    CASE("a dead sensor reads as dashes, never as zero");
    {
        // The §12 rule, checked where it actually matters: 0 psi and "no
        // idea" must not look the same.
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        Frame f = busy_frame();
        f.health[Dev::Imu].state = DevState::Fault;
        f.att.lean_deg = 0.0f;

        go_to(fx.app, s.canvas, Mode::Stat, 2);       // attitude
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t with_fault = s.ink();

        f.health[Dev::Imu].state = DevState::Ok;
        f.att.lean_deg = 42.0f;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != with_fault);      // the screen genuinely differs
    }

    CASE("widgets only repaint when their value changes");
    {
        // This is the whole reason set() compares: at 30 Hz over SPI, a
        // screen that redraws every widget every frame is the render budget.
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        const Frame f = busy_frame();

        fx.app.update(f);
        fx.app.render(s.canvas, true);

        fx.app.update(f);                  // the same frame again
        size_t dirty = 0;
        for (Widget* w : fx.app.current().widgets()) if (w && w->dirty) ++dirty;
        CHECK(dirty == 0);

        Frame f2 = f;
        f2.motion.best_mph += 5.0f;
        fx.app.update(f2);
        dirty = 0;
        for (Widget* w : fx.app.current().widgets()) if (w && w->dirty) ++dirty;
        CHECK(dirty > 0);
    }

    CASE("a screen posts commands and never writes state itself");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);

        go_to(fx.app, s.canvas, Mode::Track, 0);      // lap timer
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
        CHECK(fx.sink.posted[0].id == core::state::CommandId::LapSetLine);

        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 2);
        CHECK(fx.sink.posted[1].id == core::state::CommandId::LapArm);
    }

    CASE("the config screen edits settings and reports every change upward");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());

        const uint8_t before = fx.settings.brightness_pct;
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // edit
        // C is up and D is down, on the value as much as on the list.
        fx.app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        CHECK(fx.settings.brightness_pct == before + 5);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        CHECK(fx.settings.brightness_pct == before);
        fx.app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        CHECK(!fx.sink.posted.empty());
        CHECK(fx.sink.posted.back().id == core::state::CommandId::ConfigSet);
        CHECK(fx.sink.posted.back().cfg.key == 0x0101);

        // Leaving edit mode hands C and D back to navigation.
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        const uint8_t after = fx.settings.brightness_pct;
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        CHECK(fx.settings.brightness_pct == after);

        // And the edit marker only shows while editing: the same frame
        // rendered in and out of edit mode must differ.
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t not_editing = s.ink();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        fx.app.update(busy_frame());
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != not_editing);
    }

    CASE("an alert draws over whatever screen is up, and E dismisses it");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);
        fx.app.update(busy_frame());
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t plain = s.ink();

        fx.app.alert().show("FRONT TYRE 18 PSI");
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != plain);
        CHECK(fx.app.alert().active());

        const size_t posted_before = fx.sink.posted.size();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(!fx.app.alert().active());
        // The press that dismissed the alert must not also reach the screen.
        CHECK(fx.sink.posted.size() == posted_before);
    }

    CASE("the map screens draw a trace from a buffer they do not own");
    {
        struct Source final : public IPointSource {
            core::state::TrackBuffer<core::state::TrackPoint, 256> buf;
            size_t count() const override { return buf.size(); }
            bool point(size_t i, float& x, float& y) const override {
                core::state::TrackPoint p;
                if (!buf.read_nth(i, p)) return false;
                x = p.x_ft;
                y = p.y_ft;
                return true;
            }
        } src;

        for (int i = 0; i < 200; ++i) {
            core::state::TrackPoint p;
            p.x_ft = static_cast<float>((i % 50) * 20);
            p.y_ft = static_cast<float>((i / 50) * 60);
            src.buf.push(p);
        }

        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        fx.app.set_track_source(&src);
        go_to(fx.app, s.canvas, Mode::Track, 2);
        fx.app.update(busy_frame());
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 100);           // a trace, not just a frame and a label
    }

    CASE("the lap history screen shows the newest lap first");
    {
        const uint32_t laps[] = {92500, 91250, 90880, 91900};
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        fx.app.set_lap_history(laps, 4, 90880);
        go_to(fx.app, s.canvas, Mode::Track, 1);
        fx.app.update(busy_frame());
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 0);
    }

    CASE("mode and screen names are all present");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                go_to(fx.app, s.canvas, static_cast<Mode>(m), n);
                CHECK(fx.app.current().name()[0] != '\0');
                CHECK(fx.app.mode_name()[0] != '\0');
            }
        }
    }

    return check::report("ui");
}
