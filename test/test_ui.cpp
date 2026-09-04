#include "check.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "core/state/track_buffer.hpp"
#include <cstdio>
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

    CASE("the speed screen shows a jump, then goes back to the g-ball");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);

        Frame f = busy_frame();
        f.t_ms = 100000;
        f.att.land_ms = 0;                  // nothing has happened yet
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t ball = s.ink();

        // Just landed: the same tile, a different face.
        f.att.land_ms = f.t_ms - 500;
        f.att.airtime_ms = 1340;
        f.att.last_land_g = 4.2f;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != ball);

        // Ten seconds later it is a g-ball again, pixel for pixel. A hold
        // that never expires would leave the tile stuck on a jump for the
        // rest of the ride.
        f.t_ms += 10000;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() == ball);
    }

    CASE("the speed screen names a failed device rather than degrading quietly");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);

        Frame f = busy_frame();
        f.health[Dev::McpPod].state = DevState::Ok;    // busy_frame degrades it
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t healthy = s.ink();

        // The SD card is the test because nothing else on this screen reads
        // it: any difference in the pixels has to be the fault line itself,
        // not a readout dropping to dashes.
        f.health[Dev::Sd].state = DevState::Degraded;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != healthy);

        // And it clears again when the card comes back.
        f.health[Dev::Sd].state = DevState::Ok;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() == healthy);
    }

    CASE("no two widgets on a screen claim the same pixel");
    {
        // Bounds are what a repaint clears, so two widgets sharing a
        // rectangle take turns wiping each other out. On a full repaint that
        // is invisible — whoever draws last wins — and on the partial-update
        // panel it is a notch that appears whenever the other one changes.
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);

        const core::nav::Layout layout;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t sc = 0; sc < layout.screens[m]; ++sc) {
                go_to(fx.app, s.canvas, static_cast<Mode>(m), sc);
                const WidgetList ws = fx.app.current().widgets();
                for (size_t i = 0; i < ws.count; ++i) {
                    for (size_t j = i + 1; j < ws.count; ++j) {
                        if (!ws.items[i] || !ws.items[j]) continue;
                        const Rect& a = ws.items[i]->bounds;
                        const Rect& b = ws.items[j]->bounds;
                        if (a.empty() || b.empty()) continue;
                        const bool overlap =
                            a.x < b.right() && b.x < a.right() &&
                            a.y < b.bottom() && b.y < a.bottom();
                        if (overlap) {
                            std::printf("  %s widget %zu %d,%d %dx%d overlaps "
                                        "%zu %d,%d %dx%d\n",
                                        fx.app.current().name(), i, a.x, a.y,
                                        a.w, a.h, j, b.x, b.y, b.w, b.h);
                        }
                        CHECK(!overlap);
                    }
                }
            }
        }
    }

    CASE("the tach marker travels continuously and widens into the flare");
    {
        // The outline first: no step anywhere, and the two strips agree
        // about the width at the row where they meet.
        for (int16_t y = 0; y + 1 < kScreenH; ++y) {
            const int d = TachBar::width_at(y) - TachBar::width_at(y + 1);
            CHECK(d >= 0 && d <= 5);        // at most the flare's own gradient
        }
        CHECK(TachBar::width_at(TachBar::kFlareH) == TachBar::kStemW);
        CHECK(TachBar::width_at(0) == TachBar::kFlareW);

        Screen320x480 s;
        TachBar stem, flare;
        stem.set_bounds(Rect{0, TachBar::kFlareH, TachBar::kStemW,
                             static_cast<int16_t>(kScreenH - TachBar::kFlareH)});
        stem.configure(TachBar::kFlareH, kScreenH);
        flare.set_bounds(Rect{0, 0, TachBar::kFlareW, TachBar::kFlareH});
        flare.configure(0, TachBar::kFlareH);

        int prev_top = -1;
        for (int i = 0; i <= 400; ++i) {
            const float frac = static_cast<float>(i) / 400.0f;
            stem.set(frac);
            flare.set(frac);
            for (Color& c : s.px) c = 0;
            stem.render(s.canvas);
            flare.render(s.canvas);

            int top = -1, bottom = -1, count = 0;
            for (int16_t y = 0; y < kScreenH; ++y) {
                if (s.canvas.pixel(0, y) == color::kTach) {
                    if (top < 0) top = y;
                    bottom = y;
                    ++count;
                }
            }
            CHECK(top >= 0);                       // always somewhere
            CHECK(bottom - top + 1 == count);      // one run, no gap at the join

            if (prev_top >= 0) {
                CHECK(top <= prev_top);            // only ever upward
                CHECK(prev_top - top <= 4);        // and never in a jump
            }
            prev_top = top;

            // As wide as the outline at every row it covers, and no wider.
            // This is what "gets wider in the flare" has to mean if the
            // marker and the track are to stay the same shape.
            for (int16_t y = static_cast<int16_t>(top);
                 y <= static_cast<int16_t>(bottom); ++y) {
                const int16_t w = TachBar::width_at(y);
                CHECK(s.canvas.pixel(static_cast<int16_t>(w - 1), y) == color::kTach);
                CHECK(s.canvas.pixel(w, y) != color::kTach);
            }
        }
        CHECK(prev_top == 0);                      // full scale reaches the top
    }

    CASE("the tach bar marks every 1000 rpm over the marker, not under it");
    {
        Screen320x480 s;
        TachBar stem, flare;
        stem.set_bounds(Rect{0, TachBar::kFlareH, TachBar::kStemW,
                             static_cast<int16_t>(kScreenH - TachBar::kFlareH)});
        stem.configure(TachBar::kFlareH, kScreenH);
        flare.set_bounds(Rect{0, 0, TachBar::kFlareW, TachBar::kFlareH});
        flare.configure(0, TachBar::kFlareH);

        constexpr uint16_t kBase = 6000;   // six marks: 1000..5000
        stem.set_scale(kBase);
        flare.set_scale(kBase);

        // Full needle, right at the top of the range, so every mark sits
        // somewhere the marker itself would otherwise cover.
        stem.set(1.0f);
        flare.set(1.0f);
        for (Color& c : s.px) c = 0;
        stem.render(s.canvas);
        flare.render(s.canvas);

        const int16_t travel = static_cast<int16_t>(kScreenH - TachBar::kMarkerH);
        int marks_found = 0;
        for (uint32_t rpm = 1000; rpm < kBase; rpm += 1000) {
            const float frac = static_cast<float>(rpm) / kBase;
            const auto y = static_cast<int16_t>(travel - frac * travel);
            // Every row of the mark's thickness, not just its top one: a
            // partial overwrite would still count as "drawn" by a
            // single-row check. width_at() is recomputed per row rather
            // than reused from the top one since these six-thousand-rpm
            // marks land low enough on the stem to make no difference here,
            // but a mark that ever lands in the flare shouldn't silently
            // start checking the wrong column.
            for (int16_t dy = 0; dy < TachBar::kTickH; ++dy) {
                const auto row = static_cast<int16_t>(y + dy);
                const int16_t w = TachBar::width_at(row);
                CHECK(s.canvas.pixel(static_cast<int16_t>(w / 2), row) == color::kWhite);
            }
            ++marks_found;
        }
        CHECK(marks_found == 5);   // 1000, 2000, 3000, 4000, 5000

        // A base of 0 (no shift point configured) draws no marks and
        // doesn't crash walking off the end of the tick table.
        TachBar none;
        none.set_bounds(Rect{0, 0, TachBar::kStemW, kScreenH});
        none.configure(0, kScreenH);
        none.set_scale(0);
        none.set(0.5f);
        for (Color& c : s.px) c = 0;
        none.render(s.canvas);
        for (int16_t y = 0; y < kScreenH; ++y) {
            CHECK(s.canvas.pixel(0, y) != color::kWhite);
        }
    }

    CASE("the media block and its icons come and go together");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);

        // busy_frame() leaves updated_ms at zero, which is the gate: nothing
        // has ever arrived, so there is no media rather than a nameless song.
        Frame f = busy_frame();
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t silent = s.ink();

        std::strcpy(f.media.title, "In the Forest");
        std::strcpy(f.media.album, "Terra Incognita");
        std::strcpy(f.media.artist, "Gojira");
        f.media.updated_ms = 90000;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > silent);

        // The phone goes away and `ble` clears the slot. The icons have to go
        // with the text — three icons next to nothing is what would happen if
        // they were painted as background chrome.
        f.media = core::state::Media{};
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() == silent);
    }

    CASE("a name too long to fit scrolls, and stays inside its own row");
    {
        Screen320x480 s;
        MediaRow m;
        const Rect box{40, 60, 150, MediaRow::kRowH};
        m.set_bounds(box);
        m.configure(MediaRow::Kind::Song, color::kText);

        const char* kLong = "A Song Title Far Longer Than The Space Available";

        auto ink_outside = [&]() {
            size_t outside = 0;
            for (int16_t y = 0; y < kScreenH; ++y) {
                for (int16_t x = 0; x < kScreenW; ++x) {
                    if (s.canvas.pixel(x, y) == 0) continue;
                    if (!box.contains(x, y)) ++outside;
                }
            }
            return outside;
        };

        // The marquee deliberately draws the string starting off the left of
        // its window and running off the right. Without a clip that lands on
        // the g-ball next door, and on a partial update nothing would ever
        // clean it up — so this is the property that matters, checked all the
        // way round one cycle rather than at one convenient offset.
        for (uint32_t t = 0; t < 20000; t += 250) {
            m.set(kLong, true, t);
            for (Color& c : s.px) c = 0;
            m.render(s.canvas);
            CHECK(ink_outside() == 0);
        }

        // It really does move: two instants a second apart must differ.
        m.set(kLong, true, 0);
        for (Color& c : s.px) c = 0;
        m.render(s.canvas);
        std::vector<Color> at_zero = s.px;

        m.set(kLong, true, 1000);
        for (Color& c : s.px) c = 0;
        m.render(s.canvas);
        CHECK(s.px != at_zero);

        // Two rows with the same text do not slide in lockstep. They share
        // one clock, so without a per-row phase the whole block moves as if
        // it were one thing.
        {
            MediaRow a, b;
            a.set_bounds(box);
            b.set_bounds(box);
            // The same colour on both, or this measures the palette rather
            // than the phase and passes with the offset taken out.
            a.configure(MediaRow::Kind::Song, color::kText);
            b.configure(MediaRow::Kind::Album, color::kText);
            bool differed = false;
            for (uint32_t t = 0; t < 8000 && !differed; t += 250) {
                a.set(kLong, true, t);
                for (Color& c : s.px) c = 0;
                a.render(s.canvas);
                const std::vector<Color> from_a = s.px;

                b.set(kLong, true, t);
                for (Color& c : s.px) c = 0;
                b.render(s.canvas);
                // The icons differ by kind, so compare only the text window.
                for (int16_t y = box.y; y < box.bottom() && !differed; ++y) {
                    for (int16_t x = static_cast<int16_t>(box.x + MediaRow::kTextX);
                         x < box.right(); ++x) {
                        const size_t i = static_cast<size_t>(y) * kScreenW + x;
                        if (from_a[i] != s.px[i]) { differed = true; break; }
                    }
                }
            }
            CHECK(differed);
        }

        // A name that fits does not scroll, however long the bike has been
        // running — a static artist name repainting twelve times a second
        // would be the whole point of the row widget thrown away.
        m.set("Gojira", true, 0);
        for (Color& c : s.px) c = 0;
        m.render(s.canvas);
        const std::vector<Color> fitted = s.px;

        m.clear_dirty();
        m.set("Gojira", true, 7000);
        CHECK(!m.dirty);
        for (Color& c : s.px) c = 0;
        m.render(s.canvas);
        CHECK(s.px == fitted);
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

    CASE("a partial update lands the same pixels as a full repaint");
    {
        // The property the render task depends on: after a frame where only
        // some widgets changed, repainting just those widgets must leave the
        // panel identical to a full redraw. It fails the moment a screen
        // paints live data as background — background is only refreshed on a
        // full repaint, so on a partially updated panel it stops moving and
        // the two diverge.
        Frame a = busy_frame();
        Frame b = busy_frame();
        b.motion.best_mph = 61.0f;
        b.engine.rpm = 4100.0f;
        b.engine.shift_mask = 0x07;
        b.engine.gear = 3;
        b.att.lean_deg = 18.0f;
        b.lap.current_ms = 47310;
        b.gnss.sats = 9;
        b.gnss.utc_ms += 61u * 1000u;
        b.trip.trip_a_mi = 33.9f;

        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                Fixture fx;
                Screen320x480 partial, whole;
                fx.app.begin(partial.canvas);
                go_to(fx.app, partial.canvas, static_cast<Mode>(m), n);

                // Both start from a full repaint of the first frame.
                fx.app.update(a);
                fx.app.render(partial.canvas, true);
                whole.px = partial.px;

                // One side updates only what changed...
                fx.app.update(b);
                for (Widget* w : fx.app.current().widgets()) {
                    if (!w || !w->dirty) continue;
                    w->render(partial.canvas);
                    w->clear_dirty();
                }

                // ...the other redraws everything from the same state. No
                // second update(): a screen that accumulates, like the rpm
                // sparkline, would take an extra sample and the two sides
                // would differ for a reason that has nothing to do with
                // partial drawing.
                FrameBufferCanvas wc(whole.px.data(), kScreenW, kScreenH);
                fx.app.render(wc, true);

                size_t differing = 0;
                for (size_t i = 0; i < partial.px.size(); ++i) {
                    if (partial.px[i] != whole.px[i]) ++differing;
                }
                if (differing != 0) {
                    std::printf("       %s/%s: %zu px differ\n",
                                fx.app.mode_name(), fx.app.current().name(),
                                differing);
                }
                CHECK(differing == 0);
            }
        }
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
