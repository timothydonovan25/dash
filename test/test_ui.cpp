#include "check.hpp"
#include "core/hr/hr.hpp"
#include "core/gearcal/gearcal.hpp"
#include "core/fusion/speed.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/shadow.hpp"
#include "ui/wire_cost.hpp"
#include "core/state/track_buffer.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
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

// The render loops reuse one buffer for every rectangle, so a pixel a widget
// does not paint goes out as whatever was there before. Zeros would hide that
// wherever the right colour happens to be black; this does not.
void junk(std::vector<Color>& buf, size_t n) {
    static uint32_t seed = 0x9E3779B9u;
    buf.resize(n);
    for (Color& c : buf) {
        seed = seed * 1664525u + 1013904223u;
        c = static_cast<Color>(seed >> 16);
    }
}

// A partial update the way the render task makes one: App::damage(), each
// rectangle drawn by draw_damage() into a canvas windowed onto just that
// rectangle, and only those pixels written to the "panel". Anything a
// widget draws outside what it reported is thrown away, as on the bike.
size_t send_damage(App& app, std::vector<Color>& panel) {
    App::Damage d[App::kMaxDamage];
    const size_t n = app.damage(d, App::kMaxDamage);
    std::vector<Color> buf;
    for (size_t i = 0; i < n; ++i) {
        const Rect r = d[i].r.clipped_to(Rect{0, 0, kScreenW, kScreenH});
        if (r.empty()) continue;
        junk(buf, static_cast<size_t>(r.w) * r.h);
        FrameBufferCanvas c(buf.data(), r.w, r.h);
        c.set_window(r.x, r.y, r.w, r.h);
        app.draw_damage(c, d[i]);
        for (int16_t y = 0; y < r.h; ++y) {
            for (int16_t x = 0; x < r.w; ++x) {
                panel[static_cast<size_t>(r.y + y) * kScreenW + r.x + x] =
                    buf[static_cast<size_t>(y) * r.w + x];
            }
        }
    }
    app.clear_damage();
    return n;
}

// send_damage() through a ui::Shadow, as render_service and demo_render do:
// each rectangle drawn, compared with what the glass holds, and only the
// changed parts packed and sent. Returns the pixels sent; `whole_px` gets
// what send_damage() would have sent for the same damage.
size_t send_damage_diffed(App& app, std::vector<Color>& panel, Shadow& sh,
                          size_t* whole_px = nullptr) {
    App::Damage d[App::kMaxDamage];
    const size_t n = app.damage(d, App::kMaxDamage);
    const uint32_t overhead = wire::overhead_px(wire::i80_8bit(10000000u));
    std::vector<Color> buf;
    size_t sent = 0;
    for (size_t i = 0; i < n; ++i) {
        const Rect r = d[i].r.clipped_to(Rect{0, 0, kScreenW, kScreenH});
        if (r.empty()) continue;
        if (whole_px) *whole_px += static_cast<size_t>(r.w) * r.h;
        junk(buf, static_cast<size_t>(r.w) * r.h);
        FrameBufferCanvas c(buf.data(), r.w, r.h);
        c.set_window(r.x, r.y, r.w, r.h);
        app.draw_damage(c, d[i]);
        Rect parts[Shadow::kMaxParts];
        const int np = sh.diff(r, buf.data(), parts, Shadow::kMaxParts, overhead);
        for (int k = 0; k < np; ++k) {
            const Rect& p = parts[k];
            Shadow::pack(r, buf.data(), p);
            for (int16_t y = 0; y < p.h; ++y) {
                for (int16_t x = 0; x < p.w; ++x) {
                    panel[static_cast<size_t>(p.y + y) * kScreenW + p.x + x] =
                        buf[static_cast<size_t>(y) * p.w + x];
                }
            }
            sent += static_cast<size_t>(p.w) * p.h;
        }
    }
    app.clear_damage();
    return sent;
}

// A full repaint through a Shadow, as the render loops make one: the whole
// frame drawn, then sent whole if the glass is unknown, or only where it
// differs from what the panel holds if it is known (a screen change).
// Returns the pixels sent.
size_t full_repaint_diffed(App& app, Screen320x480& s, Shadow& sh) {
    Screen320x480 drawn;
    app.render(drawn.canvas, true);
    app.clear_full_repaint();
    const Rect screen{0, 0, kScreenW, kScreenH};
    if (!sh.valid()) {
        s.px = drawn.px;
        sh.record(screen, drawn.px.data());
        sh.validate();
        return drawn.px.size();
    }
    const uint32_t overhead = wire::overhead_px(wire::i80_8bit(10000000u));
    size_t sent = 0;
    // Band by band, as render_service draws it.
    std::vector<Color> band;
    for (int16_t y = 0; y < kScreenH; y = static_cast<int16_t>(y + 40)) {
        const Rect r{0, y, kScreenW, 40};
        band.assign(drawn.px.begin() + static_cast<size_t>(y) * kScreenW,
                    drawn.px.begin() + static_cast<size_t>(y + 40) * kScreenW);
        Rect parts[Shadow::kMaxParts];
        const int np = sh.diff(r, band.data(), parts, Shadow::kMaxParts, overhead);
        for (int k = 0; k < np; ++k) {
            const Rect& p = parts[k];
            Shadow::pack(r, band.data(), p);
            for (int16_t yy = 0; yy < p.h; ++yy) {
                for (int16_t x = 0; x < p.w; ++x) {
                    s.px[static_cast<size_t>(p.y + yy) * kScreenW + p.x + x] =
                        band[static_cast<size_t>(yy) * p.w + x];
                }
            }
            sent += static_cast<size_t>(p.w) * p.h;
        }
    }
    return sent;
}

// One frame the way every render loop runs it (render_service, demo_render,
// sim_main): render() only when the App asks for a whole frame, otherwise
// its damage. A test that calls render() itself can hide anything the App
// does only inside render() -- the gear calibration page was built there,
// and on the board it never appeared.
void render_task_frame(App& app, Screen320x480& s, const Frame& f) {
    app.update(f);
    if (app.needs_full_repaint()) {
        app.render(s.canvas, true);
        app.clear_full_repaint();
    } else {
        send_damage(app, s.px);
    }
}

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
    f.session.dist_mi = 23.4f;
    f.session.moving_s = 3725;
    f.session.engine_s = 4100;
    f.session.top_mph = 71.0f;
    f.session.avg_mph = 22.6f;
    f.session.best_air_ms = 1240;
    f.session.jumps = 6;
    f.lap.track = 2;
    f.lap.track_recalled = true;
    f.lap.track_best_ms = 89900;
    f.lap.track_ideal_ms = 88750;
    f.lap.phase = core::lap::Phase::Timing;
    f.lap.count = 4;
    f.lap.current_ms = 42150;
    f.lap.last_ms = 91250;
    f.lap.best_ms = 90880;
    f.lap.delta_ms = -320;
    f.lap.dist_to_line_ft = 1840.0f;
    f.elec.vbat = 12.6f;
    for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
        f.health.dev[i].state = DevState::Ok;
    }
    f.health[Dev::Ecu].state = DevState::Absent;
    f.health[Dev::BleHost].state = DevState::Absent;
    f.health[Dev::McpPod].state = DevState::Degraded;
    f.health[Dev::McpPod].fault_count = 7;
    // PERF, STUNT and PHONE with something on them.
    using P = core::state::Perf;
    f.perf.phase = 1;
    f.perf.last[P::To1] = {2410, 30.0f};
    f.perf.best[P::To1] = {2330, 30.0f};
    f.perf.last[P::Ft60] = {1820, 24.5f};
    f.perf.best[P::Ft60] = {1820, 24.5f};
    f.perf.last[P::Brake1] = {1900, 41.0f};
    f.perf.best[P::Brake1] = {1900, 41.0f};
    f.perf.seq = 3;
    f.perf.event = P::Brake1;
    f.stunt.seq = 4;
    f.stunt.kind = 1;
    f.stunt.grade = 2;
    f.stunt.ms = 4120;
    f.stunt.ft = 121.0f;
    f.stunt.peak_deg = 38.0f;
    f.stunt.points = 1210;
    f.stunt.mult_x10 = 15;
    f.stunt.total = 2980;
    f.stunt.tricks = 4;
    f.stunt.best_points = 1210;
    f.stunt.longest_ms = 4120;
    f.stunt.longest_ft = 121.0f;
    f.stunt.highest_deg = 41.0f;
    f.stunt.best_combo = 2;
    f.stunt.live_deg = 2.0f;
    f.notify.seq = 2;
    f.notify.kind = 'M';
    std::snprintf(f.notify.who, sizeof f.notify.who, "Alex");
    std::snprintf(f.notify.text, sizeof f.notify.text,
                  "Running late, start without me and save a spot at the top");
    f.notify.at_ms = f.t_ms - 90000;
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
    core::hr::History      hr_history;
    App                    app;
    size_t                 applied = 0;     // posted commands apply_posted() has seen

    Fixture() : ctx{&settings, &sink, &sys}, app(ctx) {
        sys.heap_free_b = 184320;
        sys.heap_min_b = 120000;
        sys.psram_free_b = 6 * 1024 * 1024;
        sys.uptime_s = 3725;
        sys.render_hz_x10 = 299;
        sys.gnss_bad_csum = 2;
        // Off for every case but its own: a splash would stand in front of
        // the screen each case is trying to look at.
        settings.splash = false;
        app.bind_settings(&settings);
        app.set_hr_history(&hr_history);
    }
};

// What `logic` does with the config screen's edits: apply each ConfigSet,
// in order. The screen itself never writes Settings.
void apply_posted(Fixture& fx) {
    for (; fx.applied < fx.sink.posted.size(); ++fx.applied) {
        const core::state::Command& c = fx.sink.posted[fx.applied];
        if (c.id != core::state::CommandId::ConfigSet) continue;
        if (const core::config::ConfigItem* it = core::config::find(c.cfg.key)) {
            core::config::set(fx.settings, *it, c.cfg.val);
        }
    }
}

// Walk to a given mode and screen with real button presses, the way a rider
// would, rather than reaching into nav state.
void go_to(App& app, Canvas& c, Mode m, uint8_t screen) {
    for (int i = 0; i < 8 && app.nav_state().mode != m; ++i) {
        app.on_input(InputEvent{Button::A, Press::Short}, c);
    }
    if (m == Mode::Diag) {
        // DIAG is a menu: back to it, the cursor to the page, E to open it.
        if (app.nav_state().screen == screen) return;
        if (app.nav_state().screen != 0) app.on_input(InputEvent{Button::B, Press::Short}, c);
        if (screen == 0) return;
        for (int i = 0; i < 16 && app.nav_state().focus != screen - 1; ++i) {
            app.on_input(InputEvent{Button::D, Press::Short}, c);
        }
        app.on_input(InputEvent{Button::E, Press::Short}, c);
        return;
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
                CHECK(s.canvas.pixel(static_cast<int16_t>(w / 2), row) == color::kAlarm);
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
            CHECK(s.canvas.pixel(0, y) != color::kAlarm);
        }
    }

    CASE("the media progress ring fills with the fraction played, and the play/pause glyph matches state");
    {
        Screen320x480 s;
        MediaProgress p;
        const Rect box{100, 100, 80, 80};
        p.set_bounds(box);

        // Invalid: no accent colour anywhere in the tile, whatever the
        // numbers say — same "no media" gate MediaRow uses.
        p.set(30000, 120000, 1, false);
        for (Color& c : s.px) c = 0;
        p.render(s.canvas);
        bool any_accent = false;
        for (int16_t y = box.y; y < box.bottom(); ++y)
            for (int16_t x = box.x; x < box.right(); ++x)
                if (s.canvas.pixel(x, y) == color::kAccent) any_accent = true;
        CHECK(!any_accent);

        // A quarter played: a point a few degrees short of the quarter mark
        // (90 deg clockwise from twelve o'clock) is inside the filled arc,
        // and a point a few degrees past it is not.
        p.set(30000, 120000, 1, true);
        for (Color& c : s.px) c = 0;
        p.render(s.canvas);
        const int16_t cx = box.cx(), cy = box.cy();
        // Mirrors MediaProgress::render's own r: the smaller half minus its
        // 2 px margin. box is square here, so either half gives the same
        // answer.
        const int16_t r = static_cast<int16_t>(box.h / 2 - 2);
        const int16_t rr = static_cast<int16_t>(r - 3);   // mid-band, clear of rounding at either edge
        auto at_deg = [&](float deg) {
            const float rad = deg * 3.14159265f / 180.0f;
            return s.canvas.pixel(static_cast<int16_t>(cx + rr * std::sin(rad)),
                                  static_cast<int16_t>(cy - rr * std::cos(rad)));
        };
        CHECK(at_deg(80.0f) == color::kAccent);
        CHECK(at_deg(100.0f) != color::kAccent);

        // The play triangle's centroid sits in the gap between the two
        // pause bars, so this one point tells the icons apart: lit for
        // play, background for paused/stopped.
        const int16_t mid_x = static_cast<int16_t>(cx - 1);
        const int16_t mid_y = static_cast<int16_t>(cy - 16);
        p.set(0, 120000, 1, true);
        for (Color& c : s.px) c = 0;
        p.render(s.canvas);
        CHECK(s.canvas.pixel(mid_x, mid_y) == color::kText);

        p.set(0, 120000, 2, true);
        for (Color& c : s.px) c = 0;
        p.render(s.canvas);
        CHECK(s.canvas.pixel(mid_x, mid_y) != color::kText);
    }

    CASE("album art matches a baked-in album by title and stays blank otherwise");
    {
        Screen320x480 s;
        AlbumArt art;
        const Rect box{50, 50, 120, 120};
        art.set_bounds(box);

        // Not one of the handful baked into ui/album_art.hpp: blank tile,
        // same "no BLE transfer, no match, no picture" story every screen
        // gets for the other 99% of a real library.
        art.set("Some Other Band's Album", true);
        s.wipe();
        art.render(s.canvas);
        bool any_ink = false;
        for (int16_t y = box.y; y < box.bottom(); ++y)
            for (int16_t x = box.x; x < box.right(); ++x)
                if (s.canvas.pixel(x, y) != color::kBg) any_ink = true;
        CHECK(!any_ink);

        // "Terra Incognita" is one of the Gojira covers baked in by
        // tools/make_album_art.py — the exact tag ffprobe reports for that
        // album, which is what the phone actually sends as `L`.
        art.set("Terra Incognita", true);
        s.wipe();
        art.render(s.canvas);
        any_ink = false;
        for (int16_t y = box.y; y < box.bottom(); ++y)
            for (int16_t x = box.x; x < box.right(); ++x)
                if (s.canvas.pixel(x, y) != color::kBg) any_ink = true;
        CHECK(any_ink);

        // Invalid (no media at all) blanks it even for a title that would
        // otherwise match — same gate MediaRow uses.
        art.set("Terra Incognita", false);
        s.wipe();
        art.render(s.canvas);
        any_ink = false;
        for (int16_t y = box.y; y < box.bottom(); ++y)
            for (int16_t x = box.x; x < box.right(); ++x)
                if (s.canvas.pixel(x, y) != color::kBg) any_ink = true;
        CHECK(!any_ink);
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

    CASE("a wrapped row breaks a long line at a space onto two, and never scrolls");
    {
        Screen320x480 s;
        MediaRow m;
        // The media screen's lyric row: 419 px of text, 34 characters a
        // line at scale 2.
        const Rect box{20, 286, 440, 33};
        m.set_bounds(box);
        m.configure(MediaRow::Kind::Lyric, color::kAccent, 2, true);

        auto draw = [&](const char* t, uint32_t now) {
            m.set(t, true, now);
            for (Color& c : s.px) c = 0;
            m.render(s.canvas);
            return s.px;
        };
        // Rightmost ink on the line starting at row y0, inside the row's
        // text window (the icon is left of it). The lyric is drawn smooth
        // (Canvas::set_smooth_small), so its edges are blends, not kAccent.
        auto right_on = [&](const std::vector<Color>& px, int16_t y0) {
            int16_t right = -1;
            for (int16_t y = y0; y < y0 + 14; ++y)
                for (int16_t x = box.x + MediaRow::kTextX; x < kScreenW; ++x)
                    if (px[static_cast<size_t>(y) * kScreenW + x] != color::kBg &&
                        px[static_cast<size_t>(y) * kScreenW + x] != 0 && x > right) right = x;
            return right;
        };
        // A smoothed glyph's ink sits inside the bitmap's cell and may stop a
        // few pixels short of its edge; a break in the wrong place is a
        // whole word off, which this still catches.
        auto ends_at = [](int16_t got, int want) { return got <= want && got >= want - 4; };
        const int16_t tx = box.x + MediaRow::kTextX;
        const int16_t y1 = box.y + 1, y2 = y1 + MediaRow::kWrapLineH * 2;

        // 59 characters, the most the phone sends; the break falls on the
        // last space inside 34.
        const char* kLong = "Through the fire and the flames we carry on, far away";
        const std::vector<Color> first = draw(kLong, 0);
        CHECK(draw(kLong, 1000) == first);   // still
        CHECK(draw(kLong, 7777) == first);
        CHECK(ends_at(right_on(first, y1),
                      tx + text_width_px("Through the fire and the flames we", 2) - 1));
        CHECK(ends_at(right_on(first, y2), tx + text_width_px("carry on, far away", 2) - 1));

        // A line that fits stays on one.
        const std::vector<Color> one = draw("Short line", 0);
        CHECK(ends_at(right_on(one, y1), tx + text_width_px("Short line", 2) - 1));
        CHECK(right_on(one, y2) == -1);
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
        // This is the whole reason set() compares: at 60 Hz over the 8080 bus, a
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

        go_to(fx.app, s.canvas, Mode::Track, 1);      // lap timer
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
        CHECK(fx.sink.posted[0].id == core::state::CommandId::LapSetLine);

        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 2);
        CHECK(fx.sink.posted[1].id == core::state::CommandId::LapArm);
    }

    CASE("RACE sets and arms the line with E, as LAP does");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Track, 0);
        CHECK(fx.app.current_id() == ScreenId::Race);
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 2);
        CHECK(fx.sink.posted[0].id == core::state::CommandId::LapSetLine);
        CHECK(fx.sink.posted[1].id == core::state::CommandId::LapArm);
    }

    CASE("RACE's rev bar is lit the colour the shift lamps are saying");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Track, 0);
        Frame f = busy_frame();
        f.engine.rpm = 5000.0f;
        auto px = [&](int x, int y) { return s.px[static_cast<size_t>(y) * kScreenW + x]; };
        // Low on the bar, clear of the slip bar's band, the 1000s marks and
        // the rpm readout over the left end (RevBar::show_number).
        const int x = 180, y = 45;
        f.engine.shift_mask = 0;
        f.engine.shift_flash = false;
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        CHECK(px(x, y) == color::kAccent);
        f.engine.shift_mask = 0x0F;
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        CHECK(px(x, y) == color::kWarn);
        f.engine.shift_flash = true;
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        CHECK(px(x, y) == color::kAlarm);
        // Past the fill is the unlit track: 5000 of 9800 is about half.
        CHECK(px(400, y) == color::kPanel);
    }

    CASE("RACE shows the rpm as a number over the bar, to the nearest 100");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 partial;
        fx.app.begin(partial.canvas);
        go_to(fx.app, partial.canvas, Mode::Track, 0);
        Frame f = busy_frame();
        f.engine.rpm = 7234.0f;
        fx.app.update(f);
        fx.app.render(partial.canvas, true);
        fx.app.clear_full_repaint();
        // White digits over the left end of the bar.
        size_t white = 0;
        for (int y = 6; y < 56; ++y)
            for (int x = 6; x < 160; ++x)
                white += partial.px[static_cast<size_t>(y) * kScreenW + x] == color::kWhite;
        CHECK(white > 50);
        const auto same_as_whole = [&]() {
            Screen320x480 whole;
            fx.app.render(whole.canvas, true);
            return whole.px == partial.px;
        };
        // Inside the same 100: nothing to send for the number.
        f.engine.rpm = 7240.0f;
        fx.app.update(f);
        App::Damage d[App::kMaxDamage];
        CHECK(fx.app.damage(d, App::kMaxDamage) == 0);
        // Into the next: just the number's box (the edge did not move).
        f.engine.rpm = 7262.0f;
        fx.app.update(f);
        const size_t n = fx.app.damage(d, App::kMaxDamage);
        CHECK(n >= 1);
        send_damage(fx.app, partial.px);
        CHECK(same_as_whole());
        // Shifting: the fill and the number together, still right.
        f.engine.rpm = 5100.0f;
        f.engine.rpm_peak = 7300.0f;
        fx.app.update(f);
        send_damage(fx.app, partial.px);
        CHECK(same_as_whole());
    }

    CASE("RACE's rev bar sends only the columns its edge crossed");
    {
        RevBar bar;
        bar.set_bounds(Rect{6, 6, 468, 50});
        bar.set_scale(10000);
        bar.set(5000.0f, false, false);
        bar.clear_dirty();
        bar.set(5200.0f, false, false);
        CHECK(bar.dirty);
        Rect r[4];
        CHECK(bar.damage(r, 4) == 1);
        CHECK(r[0].w <= 12);
        CHECK(r[0].h == 50);
        bar.clear_dirty();
        bar.set(5201.0f, false, false);              // under a step: nothing
        CHECK(!bar.dirty);
        bar.set(5200.0f, true, false);               // a lamp: the colour changed
        CHECK(bar.damage(r, 4) == 1);
        CHECK(r[0].w == 468);
    }

    CASE("SESSION shows the ride and the saved track");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Track, 4);
        CHECK(fx.app.current_id() == ScreenId::Session);
        Frame f = busy_frame();
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t busy = s.ink();
        f = dead_frame();
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() < busy);       // dashes are less ink than numbers
    }

    CASE("the speed screen turns C/D/short-E into media control, and leaves long E and A/B alone");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);   // SPEED

        fx.app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
        CHECK(fx.sink.posted[0].id == core::state::CommandId::MediaCtrl);
        CHECK(fx.sink.posted[0].i == 0);   // previous

        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 2);
        CHECK(fx.sink.posted[1].i == 1);   // play/pause

        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 3);
        CHECK(fx.sink.posted[2].i == 2);   // next

        // Long E is still the overlay menu toggle, not a fourth media command.
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        CHECK(fx.sink.posted.size() == 3);

        // A and B still switch mode/screen — this screen only claims C/D/short-E.
        fx.app.on_input(InputEvent{Button::B, Press::Short}, s.canvas);
        CHECK(fx.app.nav_state().screen != 0);
        CHECK(fx.sink.posted.size() == 3);
    }

    CASE("the media screen has its own C/D/short-E control, and renders bigger than the speed screen's block");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 1);   // MEDIA, right after SPEED

        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
        CHECK(fx.sink.posted[0].id == core::state::CommandId::MediaCtrl);
        CHECK(fx.sink.posted[0].i == 2);   // next

        Frame f = busy_frame();
        std::strcpy(f.media.title, "In the Forest");
        std::strcpy(f.media.album, "Terra Incognita");
        std::strcpy(f.media.artist, "Gojira");
        std::strcpy(f.media.lyric, "Standing here, alive");
        f.media.updated_ms = 90000;
        f.media.duration_ms = 225000;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 0);

        // "Terra Incognita" is one of the baked-in Gojira covers, so the art
        // tile between the speed digits and the ring must actually have
        // drawn something, not just left its background fill.
        bool art_ink = false;
        for (int16_t y = 30; y < 150; ++y)
            for (int16_t x = 170; x < 290; ++x)
                if (s.canvas.pixel(x, y) != color::kBg) art_ink = true;
        CHECK(art_ink);

        // The lyric row (icon + scrolling text, same as song/album/artist)
        // must have drawn too.
        bool lyric_ink = false;
        for (int16_t y = 290; y < 318; ++y)
            for (int16_t x = 20; x < 460; ++x)
                if (s.canvas.pixel(x, y) != color::kBg) lyric_ink = true;
        CHECK(lyric_ink);
    }

    CASE("the config screen posts every edit upward, and never writes Settings");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());

        // CONFIG opens on its submenus; Display is the first.
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // open
        const uint8_t before = fx.settings.brightness_pct;
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // edit
        // C is up and D is down, on the value as much as on the list. The
        // edit is logic's to make: until it does, Settings is untouched.
        fx.app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        CHECK(fx.settings.brightness_pct == before);
        apply_posted(fx);
        CHECK(fx.settings.brightness_pct == before + 5);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        apply_posted(fx);
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
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // open
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

    CASE("two presses before logic applies the first are two steps, not one");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // Display
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // edit
        const uint8_t before = fx.settings.brightness_pct;
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);   // logic not yet run
        CHECK(fx.sink.posted.size() >= 2);
        CHECK_NEAR(fx.sink.posted.back().cfg.val, before - 10.0, 1e-4);
        apply_posted(fx);
        CHECK(fx.settings.brightness_pct == before - 10);
        // And once applied, the next press steps on from what Settings says.
        fx.app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        apply_posted(fx);
        CHECK(fx.settings.brightness_pct == before - 5);
    }

    CASE("a refused Set line toasts why, and only once");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        auto strip = [&] {
            size_t n = 0;
            s.wipe();
            fx.app.render(s.canvas, true);
            for (int16_t y = 270; y < 316; ++y)
                for (int16_t x = 20; x < 460; ++x)
                    if (s.canvas.pixel(x, y) != color::kBg) ++n;
            return n;
        };
        auto f = busy_frame();
        f.t_ms += 5000;
        fx.app.update(f);
        const size_t plain = strip();
        f.t_ms += 20;
        f.lap.line_refused_seq = 1;
        fx.app.update(f);
        CHECK(strip() != plain);
        // Gone after its 3 s, and the same count does not bring it back.
        f.t_ms += 3500;
        fx.app.update(f);
        CHECK(strip() == plain);
    }

    CASE("CONFIG's SD card submenu posts Eject");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());

        size_t g = 0;
        while (g < core::config::group_count() &&
               std::strcmp(core::config::group_name(g), "SD card") != 0) ++g;
        CHECK(g < core::config::group_count());
        for (size_t i = 0; i < g; ++i)
            fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // open
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // eject
        CHECK(!fx.sink.posted.empty());
        CHECK(fx.sink.posted.back().id == core::state::CommandId::StorageEject);
    }

    CASE("config submenus: E opens one, B goes back to it, C/D stay in it");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());
        const uint8_t groups = fx.app.current().focus_count();
        CHECK(groups == core::config::group_count());

        // Down to the second submenu (Startup), open it, and the cursor and
        // the row count are that submenu's.
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.app.current().focus_count() == core::config::group_size(1));
        CHECK(fx.app.nav_state().focus == 0);

        // Toggling the sweep off goes up as a ConfigSet for its key.
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // edit
        fx.app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        apply_posted(fx);
        CHECK(!fx.settings.gauge_sweep);
        CHECK(fx.sink.posted.back().cfg.key == 0x0B01);

        // B ends the edit, then leaves the submenu with the cursor on it.
        fx.app.on_input(InputEvent{Button::B, Press::Short}, s.canvas);
        CHECK(fx.app.current().focus_count() == core::config::group_size(1));
        fx.app.on_input(InputEvent{Button::B, Press::Short}, s.canvas);
        CHECK(fx.app.current().focus_count() == groups);
        CHECK(fx.app.nav_state().focus == 1);
        CHECK(fx.app.nav_state().mode == Mode::Config);
    }

    CASE("the gear calibration page takes over CONFIG while a run is open, and hands it back");
    {
        using core::gearcal::Phase;
        using core::gearcal::Reason;
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Config, 0);
        fx.app.update(busy_frame());

        // CONFIG -> Gearing -> Calibrate gears posts the command...
        size_t g = 0;
        while (g < core::config::group_count() &&
               std::strcmp(core::config::group_name(g), "Gearing") != 0) ++g;
        CHECK(g < core::config::group_count());
        for (size_t i = 0; i < g; ++i)
            fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);   // open Gearing
        size_t row = 0;
        while (row < core::config::group_size(g) &&
               core::config::group_item(g, row)->action != core::state::CommandId::GearCalStart) ++row;
        CHECK(row < core::config::group_size(g));
        for (size_t i = 0; i < row; ++i)
            fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::GearCalStart);

        // ...and when `logic` says a run is open, the page replaces CONFIG.
        Frame f = busy_frame();
        f.gear_cal.phase = static_cast<uint8_t>(Phase::Intro);
        f.gear_cal.count = 5;
        f.gear_cal.autotune = true;
        f.gear_cal.learn_n[2] = 5;
        f.gear_cal.learn_err_pct[2] = -3.2f;
        render_task_frame(fx.app, s, f);
        render_task_frame(fx.app, s, f);
        CHECK(std::strcmp(fx.app.current().name(), "GEAR CAL") == 0);
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 500);

        // Every button belongs to it: A must not leave with a run open.
        fx.sink.posted.clear();
        fx.app.on_input(InputEvent{Button::A, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.empty());
        CHECK(fx.app.nav_state().mode == Mode::Config);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::GearCalNext);
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::GearCalNext);  // no skip on the intro

        // Measuring, ready; then the results.
        f.gear_cal.phase = static_cast<uint8_t>(Phase::Measuring);
        f.gear_cal.gear = 2;
        f.gear_cal.ready = true;
        f.gear_cal.steady_ds = 123;
        f.gear_cal.need_ds = 100;
        f.gear_cal.live_ratio = 281.4f;
        f.gear_cal.live_err_pct = -3.0f;
        f.gear_cal.done_mask = 0x01;
        f.gear_cal.result_ratio[0] = 410.2f;
        f.gear_cal.result_err_pct[0] = -2.3f;
        fx.app.update(f);
        fx.app.render(s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::GearCalSkip);
        fx.app.on_input(InputEvent{Button::B, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::GearCalCancel);

        // A partial update of the measuring page lands the same pixels as a
        // full repaint of it.
        {
            Screen320x480 whole;
            fx.app.update(f);
            fx.app.render(s.canvas, true);
            whole.px = s.px;
            Frame f2 = f;
            f2.gear_cal.steady_ds = 140;
            f2.gear_cal.live_err_pct = -2.8f;
            f2.gear_cal.ready = false;
            f2.gear_cal.reason = static_cast<uint8_t>(Reason::Unsteady);
            fx.app.update(f2);
            send_damage(fx.app, s.px);
            FrameBufferCanvas wc(whole.px.data(), kScreenW, kScreenH);
            fx.app.render(wc, true);
            size_t differing = 0;
            for (size_t i = 0; i < s.px.size(); ++i) if (s.px[i] != whole.px[i]) ++differing;
            CHECK(differing == 0);
        }

        f.gear_cal.phase = static_cast<uint8_t>(Phase::Summary);
        f.gear_cal.circ_valid = true;
        f.gear_cal.circ_err_pct = 3.1f;
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 500);

        // Closed again: CONFIG is back, in the Gearing submenu it left.
        f.gear_cal = core::state::GearCal{};
        render_task_frame(fx.app, s, f);
        render_task_frame(fx.app, s, f);
        CHECK(std::strcmp(fx.app.current().name(), "CONFIG") == 0);
        CHECK(fx.app.current().focus_count() == core::config::group_size(g));
    }

    CASE("at key-on the speed screen sweeps, then shows the bike");
    {
        core::state::Frame parked;                     // a bike at rest
        parked.t_ms = 400;

        // What the screen looks like with the sweep turned off.
        size_t at_rest = 0;
        {
            Fixture off;
            off.settings.gauge_sweep = false;
            Screen320x480 s;
            off.app.begin(s.canvas);
            go_to(off.app, s.canvas, Mode::Cruise, 0);
            off.app.update(parked);
            s.wipe();
            off.app.render(s.canvas, true);
            at_rest = s.ink();
        }

        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);
        fx.app.update(parked);                         // the sweep starts

        parked.t_ms = 400 + fx.settings.sweep_ms / 2;  // the top of the sweep
        fx.app.update(parked);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != at_rest);

        parked.t_ms = 400 + fx.settings.sweep_ms;      // over, for good
        fx.app.update(parked);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() == at_rest);
    }

    CASE("the splash covers key-on, painted once, then only its bar moves");
    {
        Fixture fx;
        fx.settings.splash = true;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        core::state::Frame f;
        f.t_ms = 1000;
        fx.app.update(f);
        CHECK(fx.app.splashing());
        CHECK(fx.app.needs_full_repaint());       // the card goes whole once
        fx.app.render(s.canvas, true);
        fx.app.clear_full_repaint();
        CHECK(s.ink() > 0);
        f.t_ms = 1000 + App::kSplashMs / 2;
        fx.app.update(f);
        CHECK(fx.app.splashing());
        CHECK(!fx.app.needs_full_repaint());      // not a 31 ms frame each time
        CHECK(send_damage(fx.app, s.px) == 1);    // just the progress bar
        Screen320x480 whole;
        fx.app.render(whole.canvas, true);
        size_t differing = 0;
        for (size_t i = 0; i < s.px.size(); ++i) if (s.px[i] != whole.px[i]) ++differing;
        CHECK(differing == 0);
        f.t_ms = 1000 + App::kSplashMs;
        fx.app.update(f);
        CHECK(!fx.app.splashing());
        CHECK(fx.app.needs_full_repaint());       // the screen comes in whole
    }

    CASE("NAV: drop and clear post commands, arrow and distance from the frame");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 4);
        CHECK(std::strcmp(fx.app.current().name(), "NAV") == 0);
        CHECK(fx.app.current().focus_count() == 5);      // START TRUCK CAMP FUEL MARK
        fx.sink.posted.clear();
        // Opens on START: short E puts it here.
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::NavDropWaypoint);
        CHECK(fx.sink.posted.back().i == 0);
        // D to CAMP, then long E clears that one.
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::E, Press::Long}, s.canvas);
        CHECK(fx.sink.posted.back().id == core::state::CommandId::NavClearWaypoint);
        CHECK(fx.sink.posted.back().i == 2);

        // No waypoint draws, and so does one 1000 ft ahead.
        core::state::Frame f;
        f.t_ms = 50;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t none = s.ink();
        f.gnss.fix = true;
        f.gnss.lat = 40.0;
        f.gnss.lon = -75.0;
        // CAMP, which is where the cursor is.
        f.waypoint.pts[2] = core::state::Waypoint{true, 40.0 + 1000.0 / 364000.0, -75.0, 0, 0};
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != none);
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

    CASE("the alerter raises the overlay from the frame, and a dismissed fault stays dismissed");
    {
        Fixture fx;
        fx.settings.splash = false;
        fx.settings.gauge_sweep = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 0);
        core::state::Frame f = busy_frame();
        f.health[core::state::Dev::Sd].state = core::state::DevState::Fault;
        for (uint32_t t = 0; t < 400; t += 20) { f.t_ms = t; fx.app.update(f); }
        CHECK(!fx.app.alert().active());               // not yet: it must persist first
        for (uint32_t t = 400; t < 1000; t += 20) { f.t_ms = t; fx.app.update(f); }
        CHECK(fx.app.alert().active());

        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(!fx.app.alert().active());
        for (uint32_t t = 1000; t < 6000; t += 20) { f.t_ms = t; fx.app.update(f); }
        CHECK(!fx.app.alert().active());               // still faulted, still dismissed

        // Fixed, then it clears the overlay by itself if it comes back and goes again.
        f.health[core::state::Dev::Sd].state = core::state::DevState::Ok;
        for (uint32_t t = 6000; t < 9000; t += 20) { f.t_ms = t; fx.app.update(f); }
        f.health[core::state::Dev::Sd].state = core::state::DevState::Fault;
        for (uint32_t t = 9000; t < 10000; t += 20) { f.t_ms = t; fx.app.update(f); }
        CHECK(fx.app.alert().active());
        fx.app.render(s.canvas, true);
        fx.app.clear_full_repaint();
        f.health[core::state::Dev::Sd].state = core::state::DevState::Ok;
        for (uint32_t t = 10000; t < 12000; t += 20) { f.t_ms = t; fx.app.update(f); }
        CHECK(!fx.app.alert().active());               // the condition ended: so does the overlay
        // ...and what it covered is repainted: that box, as damage, not the
        // whole screen.
        CHECK(!fx.app.needs_full_repaint());
        App::Damage dm[App::kMaxDamage];
        const size_t nd = fx.app.damage(dm, App::kMaxDamage);
        bool covered = false;
        for (size_t i = 0; i < nd; ++i) {
            if (dm[i].region && dm[i].r.intersects(fx.app.alert().bounds)) covered = true;
        }
        CHECK(covered);
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
        go_to(fx.app, s.canvas, Mode::Track, 3);
        fx.app.update(busy_frame());
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 100);           // a trace, not just a frame and a label

        // Drawn in 40-line bands, as the render task does, the map is the
        // same picture: every strip of one redraw shares one fit, and a
        // segment culled from a strip it does not reach is not missed.
        Screen320x480 banded;
        for (int16_t y = 0; y < kScreenH; y += 40) {
            std::vector<Color> band(static_cast<size_t>(kScreenW) * 40, 0);
            FrameBufferCanvas bc(band.data(), kScreenW, 40);
            bc.set_window(0, y, kScreenW, 40);
            fx.app.render(bc, true);
            for (int16_t r = 0; r < 40 && y + r < kScreenH; ++r) {
                for (int16_t x = 0; x < kScreenW; ++x) {
                    banded.px[static_cast<size_t>(y + r) * kScreenW + x] =
                        band[static_cast<size_t>(r) * kScreenW + x];
                }
            }
        }
        size_t differing = 0;
        for (size_t i = 0; i < s.px.size(); ++i) if (s.px[i] != banded.px[i]) ++differing;
        CHECK(differing == 0);
    }

    CASE("the lap history screen shows the newest lap first");
    {
        const uint32_t laps[] = {92500, 91250, 90880, 91900};
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        fx.app.set_lap_history(laps, 4, 90880);
        go_to(fx.app, s.canvas, Mode::Track, 2);
        fx.app.update(busy_frame());
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > 0);
    }

    CASE("the radio screen shows BLE health, RSSI, the last tag and Ctrl state");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Diag, 7);   // RADIO

        Frame f = busy_frame();
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t silent = s.ink();

        f.health[Dev::BleHost].state = DevState::Ok;
        f.health[Dev::Phone].state = DevState::Ok;
        f.ble_diag.has_rssi = true;
        f.ble_diag.rssi_dbm = -61;
        f.ble_diag.last_tag = 'P';
        f.ble_diag.ctrl_subscribed = true;
        f.ble_diag.encrypted = true;
        f.ble_diag.bonds = 1;
        f.ble_diag.pairing_s = 42;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        // Real values in place of every "--"/blank note is strictly more ink,
        // not just different ink -- a regression that silently drops back to
        // the unbuilt-firmware note would otherwise still pass an ink-only
        // inequality check.
        CHECK(s.ink() > silent);

        // An iPhone: no Ctrl subscription, AMS and ANCS instead.
        f.ble_diag.ctrl_subscribed = false;
        f.ble_diag.apple = 6;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > silent);
    }

    CASE("the update screen shows both slots and posts its three actions");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Diag, 8);   // UPDATE
        CHECK(std::strcmp(fx.app.current().name(), "UPDATE") == 0);

        Frame f = busy_frame();
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t unknown = s.ink();

        f.ota.known = true;
        f.ota.role = core::ota::Role::Stable;
        std::strcpy(f.ota.running_ver, "v1.0.0");
        std::strcpy(f.ota.running_slot, "ota_0");
        f.ota.other = core::ota::Other::Ready;
        std::strcpy(f.ota.other_ver, "v1.1.0-dev");
        std::strcpy(f.ble_diag.c6_fw, "2.12.8");
        f.ota.install = core::ota::Install::Writing;
        f.ota.install_pct = 42;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        // Not "more ink": unknown draws "stable only" on all three rows,
        // which outweighs a version and a percentage.
        CHECK(s.ink() != unknown);

        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.update(f);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        fx.app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        fx.app.update(f);
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 3);
        CHECK(fx.sink.posted[0].id == core::state::CommandId::OtaInstallSd);
        CHECK(fx.sink.posted[1].id == core::state::CommandId::OtaBootDevOnce);
        CHECK(fx.sink.posted[2].id == core::state::CommandId::OtaPromote);
    }

    CASE("a dev image wears its tag");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        Frame f = busy_frame();
        f.ota.known = true;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t stable_ink = s.ink();

        f.ota.role = core::ota::Role::Dev;
        fx.app.update(f);
        CHECK(fx.app.needs_full_repaint());
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() != stable_ink);
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
                send_damage(fx.app, partial.px);

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

    CASE("ride 300 random frames on every screen: partial always matches whole");
    {
        // The finer damage() overrides (digits, the lean marker, the nav
        // chevron, the lap time) each name only part of a widget. If any of
        // them misses a pixel that changed, the panel and a full repaint
        // stop agreeing, and stay disagreeing until something repaints
        // there; so this rides every screen through random frames and
        // compares every 25th, which keeps it to a few seconds. It found
        // the TRACK lap time drawing past its own bounds.
        uint32_t seed = 12345;
        auto rnd = [&](float lo, float hi) {
            seed = seed * 1664525u + 1013904223u;
            return lo + (hi - lo) * static_cast<float>(seed >> 8) / 16777216.0f;
        };
        const core::nav::Layout counts;
        int screens = 0;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t sn = 0; sn < counts.screens[m]; ++sn) {
                Fixture fx;
                fx.settings.gauge_sweep = false;
                Screen320x480 partial;
                fx.app.begin(partial.canvas);
                go_to(fx.app, partial.canvas, static_cast<Mode>(m), sn);
                Frame f = busy_frame();
                f.waypoint.pts[0].set = true;
                f.waypoint.pts[0].lat = f.gnss.lat + 0.01;
                f.waypoint.pts[0].lon = f.gnss.lon + 0.01;
                fx.app.update(f);
                fx.app.render(partial.canvas, true);
                fx.app.clear_full_repaint();
                size_t worst = 0;
                for (int i = 0; i < 300; ++i) {
                    f.t_ms += 17;
                    f.motion.best_mph = rnd(0.0f, 1.0f) < 0.1f ? rnd(0, 120) : f.motion.best_mph + rnd(-2, 2);
                    if (f.motion.best_mph < 0) f.motion.best_mph = 0;
                    f.engine.rpm = rnd(1200, 11000);
                    // A shift's held peak coming and going (Engine::rpm_peak).
                    if (rnd(0.0f, 1.0f) < 0.06f) {
                        f.engine.rpm_peak = rnd(0.0f, 1.0f) < 0.5f ? 0.0f : rnd(4000, 11000);
                        f.engine.shift_hold = f.engine.rpm_peak > 0.0f ? 0x20 : 0;
                    }
                    f.att.lean_deg = rnd(0.0f, 1.0f) < 0.05f ? rnd(-90, 90) : f.att.lean_deg + rnd(-4, 4);
                    f.health[Dev::Imu].state = rnd(0.0f, 1.0f) > 0.02f ? DevState::Ok : DevState::Fault;
                    f.gnss.course_deg = rnd(0, 360);
                    f.gnss.speed_mph = rnd(0, 60);
                    f.lap.current_ms += static_cast<uint32_t>(rnd(0, 40));
                    if (rnd(0.0f, 1.0f) < 0.02f) f.lap.current_ms = static_cast<uint32_t>(rnd(0, 700000));
                    f.trip.trip_a_mi += rnd(0, 0.02f);
                    fx.app.update(f);
                    if (fx.app.needs_full_repaint()) {
                        fx.app.render(partial.canvas, true);
                        fx.app.clear_full_repaint();
                    } else {
                        send_damage(fx.app, partial.px);
                    }
                    if (i % 25 != 24) continue;
                    Screen320x480 whole;
                    fx.app.render(whole.canvas, true);
                    size_t differing = 0;
                    for (size_t k = 0; k < partial.px.size(); ++k) {
                        if (partial.px[k] != whole.px[k]) ++differing;
                    }
                    if (differing > worst) worst = differing;
                }
                if (worst) {
                    std::printf("       %s/%s: up to %zu px differ\n",
                                fx.app.mode_name(), fx.app.current().name(), worst);
                }
                CHECK(worst == 0);
                ++screens;
            }
        }
        CHECK(screens >= 20);
    }

    CASE("the same ride through a shadow: still matches whole, sends less");
    {
        // ui::Shadow sends only the changed part of each damage rectangle.
        // A part it wrongly thinks unchanged is a pixel the glass never gets,
        // so the property is the one above, with the diff in the loop; and
        // it has to be worth having, so it must also send less.
        uint32_t seed = 12345;
        auto rnd = [&](float lo, float hi) {
            seed = seed * 1664525u + 1013904223u;
            return lo + (hi - lo) * static_cast<float>(seed >> 8) / 16777216.0f;
        };
        const core::nav::Layout counts;
        int screens = 0;
        size_t total_sent = 0, total_whole = 0;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t sn = 0; sn < counts.screens[m]; ++sn) {
                Fixture fx;
                fx.settings.gauge_sweep = false;
                Screen320x480 panel;
                std::vector<Color> store(static_cast<size_t>(kScreenW) * kScreenH, 0);
                Shadow sh(store.data());
                fx.app.begin(panel.canvas);
                go_to(fx.app, panel.canvas, static_cast<Mode>(m), sn);
                Frame f = busy_frame();
                f.waypoint.pts[0].set = true;
                f.waypoint.pts[0].lat = f.gnss.lat + 0.01;
                f.waypoint.pts[0].lon = f.gnss.lon + 0.01;
                fx.app.update(f);
                full_repaint_diffed(fx.app, panel, sh);
                size_t worst = 0;
                for (int i = 0; i < 300; ++i) {
                    f.t_ms += 17;
                    f.motion.best_mph = rnd(0.0f, 1.0f) < 0.1f ? rnd(0, 120) : f.motion.best_mph + rnd(-2, 2);
                    if (f.motion.best_mph < 0) f.motion.best_mph = 0;
                    f.engine.rpm = rnd(1200, 11000);
                    // A shift's held peak coming and going (Engine::rpm_peak).
                    if (rnd(0.0f, 1.0f) < 0.06f) {
                        f.engine.rpm_peak = rnd(0.0f, 1.0f) < 0.5f ? 0.0f : rnd(4000, 11000);
                        f.engine.shift_hold = f.engine.rpm_peak > 0.0f ? 0x20 : 0;
                    }
                    f.att.lean_deg = rnd(0.0f, 1.0f) < 0.05f ? rnd(-90, 90) : f.att.lean_deg + rnd(-4, 4);
                    f.health[Dev::Imu].state = rnd(0.0f, 1.0f) > 0.02f ? DevState::Ok : DevState::Fault;
                    f.gnss.course_deg = rnd(0, 360);
                    f.gnss.speed_mph = rnd(0, 60);
                    f.lap.current_ms += static_cast<uint32_t>(rnd(0, 40));
                    if (rnd(0.0f, 1.0f) < 0.02f) f.lap.current_ms = static_cast<uint32_t>(rnd(0, 700000));
                    f.trip.trip_a_mi += rnd(0, 0.02f);
                    fx.app.update(f);
                    if (fx.app.needs_full_repaint()) {
                        full_repaint_diffed(fx.app, panel, sh);
                    } else {
                        total_sent += send_damage_diffed(fx.app, panel.px, sh, &total_whole);
                    }
                    if (i % 25 != 24) continue;
                    Screen320x480 whole;
                    fx.app.render(whole.canvas, true);
                    size_t differing = 0;
                    for (size_t k = 0; k < panel.px.size(); ++k) {
                        if (panel.px[k] != whole.px[k]) ++differing;
                    }
                    if (differing > worst) worst = differing;
                }
                if (worst) {
                    std::printf("       %s/%s: up to %zu px differ\n",
                                fx.app.mode_name(), fx.app.current().name(), worst);
                }
                CHECK(worst == 0);
                ++screens;
            }
        }
        CHECK(screens >= 20);
        std::printf("       shadow: %zu of %zu damaged px sent (%.0f%%)\n", total_sent,
                    total_whole, 100.0 * static_cast<double>(total_sent) /
                                     static_cast<double>(total_whole ? total_whole : 1));
        CHECK(total_sent < total_whole);
    }

    CASE("screen changes through a shadow: every screen lands whole, for less");
    {
        // A screen change is a full repaint, and over a glass the shadow
        // knows only what differs from the screen before goes out. Walk every
        // screen of every mode the way a rider does, riding a few frames on
        // each, and after every step the panel must be what a full redraw
        // gives.
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 panel;
        std::vector<Color> store(static_cast<size_t>(kScreenW) * kScreenH, 0);
        Shadow sh(store.data());
        fx.app.begin(panel.canvas);
        Frame f = busy_frame();
        fx.app.update(f);
        full_repaint_diffed(fx.app, panel, sh);
        size_t worst = 0, changes = 0, sent = 0;
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                Screen320x480 scratch;
                go_to(fx.app, scratch.canvas, static_cast<Mode>(m), n);
                for (int i = 0; i < 3; ++i) {
                    f.t_ms += 17;
                    f.motion.best_mph += 1.0f;
                    f.engine.rpm += 150.0f;
                    fx.app.update(f);
                    if (fx.app.needs_full_repaint()) {
                        sent += full_repaint_diffed(fx.app, panel, sh);
                        ++changes;
                    } else {
                        send_damage_diffed(fx.app, panel.px, sh);
                    }
                    Screen320x480 whole;
                    fx.app.render(whole.canvas, true);
                    size_t differing = 0;
                    for (size_t k = 0; k < panel.px.size(); ++k) {
                        if (panel.px[k] != whole.px[k]) ++differing;
                    }
                    if (differing) {
                        std::printf("       %s/%s: %zu px differ\n", fx.app.mode_name(),
                                    fx.app.current().name(), differing);
                    }
                    if (differing > worst) worst = differing;
                }
            }
        }
        CHECK(worst == 0);
        CHECK(changes >= 15);
        const size_t whole_px = changes * static_cast<size_t>(kScreenW) * kScreenH;
        std::printf("       (screen changes: %zu, %zu%% of their pixels sent)\n", changes,
                    whole_px ? 100 * sent / whole_px : 0);
        CHECK(sent < whole_px);
    }

    CASE("LAP flashes a new best for five seconds, and shows a split for five");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Track, 1);
        CHECK(fx.app.current_id() == ScreenId::Lap);
        Frame f = busy_frame();
        f.lap.phase = core::lap::Phase::Timing;
        f.lap.count = 3;
        f.lap.last_ms = 90100;
        f.lap.best_ms = 90100;
        f.lap.last_was_best = true;
        f.lap.last_done_ms = 50000;
        f.lap.split_ms = 29880;
        f.lap.split_sector = 0;
        f.lap.split_delta_ms = -180;
        f.lap.split_delta_valid = true;
        f.lap.split_at_ms = 50000;
        f.lap.sector = 1;
        auto frame_at = [&](uint32_t t) {
            f.t_ms = t;
            render_task_frame(fx.app, s, f);
            return s.px;
        };
        const auto lit = frame_at(50100);       // first quarter-second: lit
        const auto dark = frame_at(50350);      // second: dark
        const auto lit2 = frame_at(50600);
        size_t blink = 0, same = 0;
        for (size_t i = 0; i < lit.size(); ++i) {
            if (lit[i] != dark[i]) ++blink;
            if (lit[i] != lit2[i]) ++same;
        }
        CHECK(blink > 100);                     // LAST went dark
        CHECK(same == 0);                       // and came back the same
        const auto during = frame_at(54000);
        const auto after = frame_at(56000);     // both over
        size_t settled = 0;
        for (size_t i = 0; i < during.size(); ++i) if (during[i] != after[i]) ++settled;
        CHECK(settled > 100);                   // the split went back to the sector
        const auto later = frame_at(58000);
        size_t still = 0;
        for (size_t i = 0; i < after.size(); ++i) if (after[i] != later[i]) ++still;
        CHECK(still == 0);                      // nothing flashing any more
    }

    CASE("IMU's lean peaks come from the Frame and survive leaving the screen");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Stat, 2);
        CHECK(fx.app.current_id() == ScreenId::Attitude);
        Frame f = busy_frame();
        f.lap.max_lean_left_deg = 0.0f;
        f.lap.max_lean_right_deg = 0.0f;
        render_task_frame(fx.app, s, f);
        const auto none = s.px;
        f.lap.max_lean_left_deg = 44.0f;
        f.lap.max_lean_right_deg = 41.0f;
        render_task_frame(fx.app, s, f);
        const auto peaks = s.px;
        size_t diff = 0;
        for (size_t i = 0; i < none.size(); ++i) if (none[i] != peaks[i]) ++diff;
        CHECK(diff > 20);
        // Away and back: the screen is rebuilt, the peaks are not lost.
        go_to(fx.app, s.canvas, Mode::Cruise, 0);
        go_to(fx.app, s.canvas, Mode::Stat, 2);
        render_task_frame(fx.app, s, f);
        size_t back = 0;
        for (size_t i = 0; i < peaks.size(); ++i) if (peaks[i] != s.px[i]) ++back;
        CHECK(back == 0);
        // Short E asks logic to reset them, rather than forgetting locally.
        fx.sink.posted.clear();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        bool asked = false;
        for (const auto& c : fx.sink.posted) {
            asked |= c.id == core::state::CommandId::LeanPeaksReset;
        }
        CHECK(asked);
    }

    CASE("DIAG opens on a menu: E opens a page, B comes back to it");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Diag, 0);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
        auto press = [&](Button b, Press p = Press::Short) {
            fx.app.on_input(InputEvent{b, p}, s.canvas);
        };
        // B on the menu walks nowhere.
        press(Button::B);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
        press(Button::B, Press::Long);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
        // Down three to Power... (Sensors, GPS, Fusion, System, Power)
        for (int i = 0; i < 4; ++i) press(Button::D);
        press(Button::E);
        CHECK(fx.app.current_id() == ScreenId::Power);
        // ...and B back, with the cursor still on it.
        press(Button::B);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
        CHECK(fx.app.nav_state().focus == 4);
        press(Button::E);
        CHECK(fx.app.current_id() == ScreenId::Power);
        // Long B from a page is back too, not the previous page.
        press(Button::B, Press::Long);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
        // C wraps to the last page: Update, whose own E still belongs to it.
        while (fx.app.nav_state().focus != 0) press(Button::C);
        press(Button::C);
        CHECK(fx.app.nav_state().focus == DiagMenuScreen::kPages - 1);
        press(Button::E);
        CHECK(fx.app.current_id() == ScreenId::Update);
        fx.sink.posted.clear();
        press(Button::E);
        CHECK(fx.sink.posted.size() == 1);
        CHECK(fx.app.current_id() == ScreenId::Update);
        // A still leaves DIAG, and coming back starts at the menu.
        press(Button::A);
        CHECK(fx.app.nav_state().mode == Mode::Config);
        for (int i = 0; i < 4; ++i) press(Button::A);
        CHECK(fx.app.current_id() == ScreenId::DiagMenu);
    }

    CASE("DIAG's menu has a program row only in a build that has one");
    {
        struct Program final : IProgram {
            int launches = 0;
            bool runnable = true;
            const char* name() const override { return "Doom"; }
            const char* state() const override { return "ready"; }
            bool launch() override { ++launches; return runnable; }
            bool active() const override { return false; }
        } prog;
        // Without one: eight rows, C from the top wraps to Update.
        {
            Fixture fx;
            Screen320x480 s;
            fx.app.begin(s.canvas);
            go_to(fx.app, s.canvas, Mode::Diag, 0);
            CHECK(fx.app.current().focus_count() == DiagMenuScreen::kPages);
        }

        Fixture fx;
        fx.ctx.program = &prog;
        App app(fx.ctx);
        app.bind_settings(&fx.settings);
        Screen320x480 s;
        app.begin(s.canvas);
        go_to(app, s.canvas, Mode::Diag, 0);
        CHECK(app.current_id() == ScreenId::DiagMenu);
        CHECK(app.current().focus_count() == DiagMenuScreen::kPages + 1);
        // C from the top wraps to it, the last row.
        app.on_input(InputEvent{Button::C, Press::Short}, s.canvas);
        CHECK(app.nav_state().focus == DiagMenuScreen::kPages);
        app.update(busy_frame());
        app.render(s.canvas, true);
        // E starts it and leaves the menu as it was: the render task takes
        // the panel from here, not the App.
        app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(prog.launches == 1);
        CHECK(app.current_id() == ScreenId::DiagMenu);
        CHECK(app.nav_state().focus == DiagMenuScreen::kPages);
        // A program that cannot start changes nothing either.
        prog.runnable = false;
        app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(prog.launches == 2);
        CHECK(app.current_id() == ScreenId::DiagMenu);
        // The pages above it still open as before.
        app.on_input(InputEvent{Button::D, Press::Short}, s.canvas);
        CHECK(app.nav_state().focus == 0);
        app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(app.current_id() == ScreenId::Sensors);
    }

    CASE("a trick toasts along the bottom, then the strip is put back");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 partial;
        fx.app.begin(partial.canvas);
        // TRIP: off SPEED, which shows tricks in its lean gauge instead.
        go_to(fx.app, partial.canvas, Mode::Cruise, 2);
        Frame f = busy_frame();
        fx.app.update(f);
        fx.app.render(partial.canvas, true);
        fx.app.clear_full_repaint();
        // What was there at the first frame is not news.
        CHECK(!fx.app.toast().active());

        const auto same_as_whole = [&]() {
            Screen320x480 whole;
            fx.app.render(whole.canvas, true);
            size_t differing = 0;
            for (size_t k = 0; k < partial.px.size(); ++k) {
                if (partial.px[k] != whole.px[k]) ++differing;
            }
            if (differing) std::printf("       %zu px differ\n", differing);
            return differing == 0;
        };
        const auto frame = [&]() {
            fx.app.update(f);
            if (fx.app.needs_full_repaint()) {
                fx.app.render(partial.canvas, true);
                fx.app.clear_full_repaint();
            } else {
                send_damage(fx.app, partial.px);
            }
        };

        f.t_ms += 20;
        f.stunt.seq += 1;
        f.stunt.points = 1840;
        f.stunt.record = true;
        frame();
        CHECK(fx.app.toast().active());
        CHECK(same_as_whole());
        // Still up a second later, gone after its 3.5 s, and the screen
        // under it is whole again without a full repaint.
        f.t_ms += 1000;
        frame();
        CHECK(fx.app.toast().active());
        f.t_ms += 3000;
        frame();
        CHECK(!fx.app.toast().active());
        CHECK(same_as_whole());

        // A wheelie under way is live in the strip, and changes as it goes.
        f.stunt.live_kind = 1;
        f.stunt.live_ms = 1200;
        f.stunt.live_deg = 36.0f;
        f.stunt.live_points = 210.0f;
        f.stunt.live_balance = true;
        f.t_ms += 20;
        frame();
        CHECK(fx.app.toast().active());
        f.stunt.live_ms = 1900;
        f.t_ms += 20;
        frame();
        CHECK(same_as_whole());
        f.stunt.live_kind = 0;
        f.t_ms += 20;
        frame();
        CHECK(!fx.app.toast().active());
        CHECK(same_as_whole());
    }

    CASE("the map's rider marker points the way the trace is going");
    {
        // A straight trace, 100 points 10 ft apart, in a direction.
        struct Line final : IPointSource {
            float dx = 0, dy = 0;
            size_t count() const override { return 100; }
            bool point(size_t i, float& x, float& y) const override {
                x = dx * 10.0f * static_cast<float>(i);
                y = dy * 10.0f * static_cast<float>(i);
                return true;
            }
        };
        // The marker's pixels (kWarn), as an offset from the dot's centre:
        // the arrowhead pulls the centroid of the whole mark its way.
        const auto lean = [](float dx, float dy, float& ox, float& oy) {
            Line line;
            line.dx = dx;
            line.dy = dy;
            MapView m;
            m.set_bounds(Rect{14, 32, 286, 276});
            m.set_source(&line);
            m.set_live(true);
            Screen320x480 s;
            m.render(s.canvas);
            double sx = 0, sy = 0;
            int n = 0;
            for (int16_t y = 0; y < kScreenH; ++y)
                for (int16_t x = 0; x < kScreenW; ++x)
                    if (s.px[static_cast<size_t>(y) * kScreenW + x] == color::kWarn) {
                        sx += x; sy += y; ++n;
                    }
            // Where the newest point lands: the far end of the trace.
            const Rect in = Rect{14, 32, 286, 276}.inset(MapView::kMarkerMargin);
            const float hx = dx > 0 ? in.right() : dx < 0 ? in.x : in.cx();
            const float hy = dy > 0 ? in.y : dy < 0 ? in.bottom() : in.cy();
            ox = n ? static_cast<float>(sx / n) - hx : 0.0f;
            oy = n ? static_cast<float>(sy / n) - hy : 0.0f;
            return n;
        };
        float ox, oy;
        CHECK(lean(1, 0, ox, oy) > 0);      // east: the nose is to the right
        CHECK(ox > 1.0f && std::fabs(oy) < 1.5f);
        CHECK(lean(0, 1, ox, oy) > 0);      // north: up the screen
        CHECK(oy < -1.0f && std::fabs(ox) < 1.5f);
        CHECK(lean(-1, 0, ox, oy) > 0);     // west
        CHECK(ox < -1.0f);
    }

    CASE("a shift leaves a held peak on SPEED's tach and RACE's bar");
    {
        for (int which = 0; which < 2; ++which) {
            Fixture fx;
            fx.settings.gauge_sweep = false;
            Screen320x480 partial;
            fx.app.begin(partial.canvas);
            if (which == 0) go_to(fx.app, partial.canvas, Mode::Cruise, 0);
            else go_to(fx.app, partial.canvas, Mode::Track, 0);
            Frame f = busy_frame();
            f.engine.rpm = 6500.0f;
            fx.app.update(f);
            fx.app.render(partial.canvas, true);
            fx.app.clear_full_repaint();
            // White pixels, less RACE's rpm readout over the bar's left end,
            // whose digits change with the rpm.
            const auto white = [&]() {
                size_t n = 0;
                for (size_t k = 0; k < partial.px.size(); ++k) {
                    const size_t x = k % kScreenW, y = k / kScreenW;
                    if (which == 1 && x < 160 && y < 60) continue;
                    n += partial.px[k] == color::kWhite;
                }
                return n;
            };
            const auto same_as_whole = [&]() {
                Screen320x480 whole;
                fx.app.render(whole.canvas, true);
                return whole.px == partial.px;
            };
            const auto frame = [&]() {
                fx.app.update(f);
                if (fx.app.needs_full_repaint()) {
                    fx.app.render(partial.canvas, true);
                    fx.app.clear_full_repaint();
                } else {
                    send_damage(fx.app, partial.px);
                }
            };
            const size_t before = white();
            f.engine.rpm_peak = 8400.0f;
            f.engine.shift_hold = 0x40;
            f.t_ms += 20;
            frame();
            CHECK(white() > before);
            CHECK(same_as_whole());
            f.engine.rpm = 7100.0f;   // pulling again under it
            f.t_ms += 20;
            frame();
            CHECK(same_as_whole());
            f.engine.rpm_peak = 0.0f; // the hold runs out
            f.engine.shift_hold = 0;
            f.t_ms += 20;
            frame();
            CHECK(white() == before);
            CHECK(same_as_whole());
        }
    }

    CASE("on SPEED a trick takes the lean gauge, not the toast");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 partial;
        fx.app.begin(partial.canvas);
        go_to(fx.app, partial.canvas, Mode::Cruise, 0);
        CHECK(fx.app.current_id() == ScreenId::Speed);
        Frame f = busy_frame();
        f.att.lean_deg = 0.0f;
        fx.app.update(f);
        fx.app.render(partial.canvas, true);
        fx.app.clear_full_repaint();
        const auto same_as_whole = [&]() {
            Screen320x480 whole;
            fx.app.render(whole.canvas, true);
            size_t differing = 0;
            for (size_t k = 0; k < partial.px.size(); ++k) {
                if (partial.px[k] != whole.px[k]) ++differing;
            }
            if (differing) std::printf("       %zu px differ\n", differing);
            return differing == 0;
        };
        const auto frame = [&]() {
            fx.app.update(f);
            if (fx.app.needs_full_repaint()) {
                fx.app.render(partial.canvas, true);
                fx.app.clear_full_repaint();
            } else {
                send_damage(fx.app, partial.px);
            }
        };
        // The gauge's band, to see what it holds.
        const Rect gauge{116, 94, 248, 126};
        const auto band = [&]() {
            std::vector<Color> b;
            for (int16_t y = gauge.y; y < gauge.bottom(); ++y)
                for (int16_t x = gauge.x; x < gauge.right(); ++x)
                    b.push_back(partial.px[static_cast<size_t>(y) * kScreenW + x]);
            return b;
        };
        const std::vector<Color> lean = band();

        // Under way: the gauge shows it, the strip does not.
        f.stunt.live_kind = 1;
        f.stunt.live_ms = 1200;
        f.stunt.live_deg = 34.0f;
        f.stunt.live_points = 300.0f;
        f.t_ms += 20;
        frame();
        CHECK(!fx.app.toast().active());
        CHECK(band() != lean);
        CHECK(same_as_whole());
        const std::vector<Color> live = band();
        // Points running up inside the same tenth: not redrawn yet.
        f.stunt.live_points = 320.0f;
        f.t_ms += 20;
        frame();
        CHECK(band() == live);
        f.stunt.live_ms = 1300;
        f.t_ms += 20;
        frame();
        CHECK(band() != live);
        CHECK(same_as_whole());

        // Scored: the result for 3.5 s, then lean again.
        f.stunt.live_kind = 0;
        f.stunt.seq += 1;
        f.stunt.done_ms = f.t_ms;
        f.stunt.points = 2219;
        f.t_ms += 20;
        frame();
        CHECK(!fx.app.toast().active());
        CHECK(band() != lean);
        CHECK(same_as_whole());
        f.t_ms += 3600;
        frame();
        CHECK(band() == lean);
        CHECK(same_as_whole());

        // Popups off: the gauge stays the gauge.
        fx.settings.popups = false;
        f.stunt.live_kind = 1;
        f.t_ms += 20;
        frame();
        CHECK(band() == lean);
        CHECK(!fx.app.toast().active());
    }

    CASE("a ringing call holds the strip; popups off silences tricks, not the phone");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        fx.settings.popups = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        Frame f = busy_frame();
        fx.app.update(f);
        f.t_ms += 20;
        f.stunt.seq += 1;
        f.perf.seq += 1;
        fx.app.update(f);
        CHECK(!fx.app.toast().active());

        f.notify.seq += 1;
        f.notify.kind = 'C';
        f.notify.ringing = true;
        f.notify.text[0] = '\0';
        f.notify.at_ms = f.t_ms;
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        f.t_ms += 30000;               // past a message's 8 s: still ringing
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        f.notify.ringing = false;      // answered
        f.t_ms += 20;
        fx.app.update(f);
        CHECK(!fx.app.toast().active());

        fx.settings.notify_on = false;
        f.notify.seq += 1;
        f.notify.kind = 'M';
        fx.app.update(f);
        CHECK(!fx.app.toast().active());
    }

    CASE("a message outranks a trick; a run toasts only when it is a best");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        Frame f = busy_frame();
        fx.app.update(f);
        // An ordinary run: on PERF, not news.
        f.t_ms += 20;
        f.perf.seq += 1;
        f.perf.event_best = false;
        fx.app.update(f);
        CHECK(!fx.app.toast().active());
        // A message, then a trick on top of it: the message stays up.
        f.notify.seq += 1;
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        const std::vector<Color> shown_msg = [&] {
            Screen320x480 a;
            fx.app.render(a.canvas, true);
            return a.px;
        }();
        f.t_ms += 500;
        f.stunt.seq += 1;
        fx.app.update(f);
        Screen320x480 b;
        fx.app.render(b.canvas, true);
        CHECK(b.px == shown_msg);
        // A text message waits: still up after 8 s, until E clears it. That
        // E is the strip's, not the screen's.
        f.t_ms += 8000;
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        fx.sink.posted.clear();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        fx.app.update(f);
        CHECK(!fx.app.toast().active());
        CHECK(fx.sink.posted.empty());
        // With nothing on the strip, E is the screen's again (SPEED: play/pause).
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
    }

    CASE("a text message stays a minute, and shows its text once stopped");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        Frame f = busy_frame();      // moving, 54 mph
        fx.app.update(f);
        f.notify.seq += 1;
        f.notify.at_ms = f.t_ms;
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        Screen320x480 moving;
        fx.app.render(moving.canvas, true);
        // Stopped: the same strip, now with the message in it.
        f.motion.best_mph = 0.0f;
        f.t_ms += 20;
        fx.app.update(f);
        Screen320x480 stopped;
        fx.app.render(stopped.canvas, true);
        // The strip only (y 266 on): stopping also redraws the speedo.
        const auto strip_ink = [](const Screen320x480& sc) {
            size_t n = 0;
            for (size_t k = static_cast<size_t>(266) * kScreenW; k < sc.px.size(); ++k)
                n += sc.px[k] != color::kBg && sc.px[k] != color::kPanel && sc.px[k] != 0;
            return n;
        };
        CHECK(strip_ink(stopped) > strip_ink(moving));
        f.t_ms += 59000;
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        f.t_ms += 1500;
        fx.app.update(f);
        CHECK(!fx.app.toast().active());
        // A missed call is not a message: no wait, and E stays the screen's.
        f.notify.seq += 1;
        f.notify.kind = 'X';
        f.notify.text[0] = '\0';
        fx.app.update(f);
        CHECK(fx.app.toast().active());
        fx.sink.posted.clear();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1);
        f.t_ms += 8100;
        fx.app.update(f);
        CHECK(!fx.app.toast().active());
    }

    CASE("PERF's E clears bests, STUNT's E starts a new score");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Stat, 3);
        CHECK(fx.app.current_id() == ScreenId::Perf);
        fx.app.update(busy_frame());
        fx.app.render(s.canvas, true);
        fx.sink.posted.clear();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1 &&
              fx.sink.posted[0].id == core::state::CommandId::PerfReset);
        go_to(fx.app, s.canvas, Mode::Stat, 4);
        CHECK(fx.app.current_id() == ScreenId::Stunt);
        fx.app.update(busy_frame());
        fx.app.render(s.canvas, true);
        fx.sink.posted.clear();
        fx.app.on_input(InputEvent{Button::E, Press::Short}, s.canvas);
        CHECK(fx.sink.posted.size() == 1 &&
              fx.sink.posted[0].id == core::state::CommandId::StuntReset);
    }

    CASE("PHONE shows the message when stopped, only the sender when moving");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Cruise, 6);
        CHECK(fx.app.current_id() == ScreenId::Phone);
        Frame f = busy_frame();
        f.motion.best_mph = 0.0f;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t stopped = s.ink();
        f.motion.best_mph = 40.0f;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() < stopped);
        fx.settings.notify_text_moving = true;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(s.ink() == stopped);
    }

    CASE("DIAG -> FUSION shows what the filter made of each source");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Diag, 3);
        CHECK(fx.app.current_id() == ScreenId::Fusion);
        Frame f = busy_frame();
        f.fused.speed_mph = 54.2f;
        f.fused.speed_valid = true;
        f.fused.used = core::fusion::kSrcFront | core::fusion::kSrcGps | core::fusion::kSrcAccel;
        f.motion.rear_mph = 58.9f;
        auto shot = [&](const Frame& fr) {
            render_task_frame(fx.app, s, fr);
            return s.px;
        };
        const auto used = shot(f);
        Frame gated = f;
        gated.fused.rejected = core::fusion::kSrcRear;
        const auto g = shot(gated);
        Frame suspect = gated;
        suspect.fused.suspect = core::fusion::kSrcRear;
        const auto sp = shot(suspect);
        size_t a = 0, b = 0;
        for (size_t i = 0; i < used.size(); ++i) {
            if (used[i] != g[i]) ++a;
            if (g[i] != sp[i]) ++b;
        }
        CHECK(a > 50);      // the rear's state changed on screen
        CHECK(b > 50);      // and again when it turned suspect
        // And a partial update still lands what a full repaint would.
        Screen320x480 whole;
        fx.app.render(whole.canvas, true);
        size_t differ = 0;
        for (size_t i = 0; i < sp.size(); ++i) if (sp[i] != whole.px[i]) ++differ;
        CHECK(differ == 0);
    }

    CASE("key-on splash and sweep: the panel matches a full repaint, frame by frame");
    {
        // Every ride case turns the sweep off, so this is the only one that
        // sends it: rpm, speed and gear to full scale and back in about a
        // second, on whatever screen the dash wakes on. Both ways the render
        // loops send it -- whole damage rectangles, and through a Shadow --
        // compared with a full repaint after every frame.
        for (int diffed = 0; diffed < 2; ++diffed) {
            Fixture fx;
            fx.settings.splash = true;
            fx.settings.gauge_sweep = true;
            Screen320x480 panel;
            std::vector<Color> store(static_cast<size_t>(kScreenW) * kScreenH, 0);
            Shadow sh(store.data());
            fx.app.begin(panel.canvas);
            Frame f = busy_frame();
            size_t worst = 0;
            int worst_at = -1;
            for (int i = 0; i < 500; ++i) {           // 4 s at 120 Hz
                f.t_ms += 8;
                fx.app.update(f);
                if (fx.app.needs_full_repaint()) {
                    full_repaint_diffed(fx.app, panel, sh);
                } else if (diffed) {
                    send_damage_diffed(fx.app, panel.px, sh);
                } else {
                    send_damage(fx.app, panel.px);
                }
                Screen320x480 whole;
                fx.app.render(whole.canvas, true);
                size_t differing = 0;
                for (size_t k = 0; k < panel.px.size(); ++k) {
                    if (panel.px[k] != whole.px[k]) ++differing;
                }
                if (differing > worst) { worst = differing; worst_at = i; }
            }
            if (worst) {
                std::printf("       %s: up to %zu px differ (first worst at frame %d)\n",
                            diffed ? "shadow" : "whole rects", worst, worst_at);
            }
            CHECK(worst == 0);
        }
    }

    CASE("riding frames send only what moved: SPEED, MINIMAL and LAP");
    {
        // The pixels a riding frame sends on the screens a rider watches,
        // averaged over two seconds of a steady pull. With the lean gauge,
        // the big digits and the running lap time sent whole, this ride was
        // SPEED ~25k px a frame, MINIMAL ~13k, LAP ~20k; now ~6k, ~2.5k and
        // ~4k (sim/wire_report has the costs in ms).
        struct Want { Mode m; uint8_t screen; const char* name; size_t max_avg_px; };
        const Want wants[] = {
            {Mode::Cruise, 0, "SPEED", 12000},
            {Mode::Cruise, 5, "MINIM", 6000},
            {Mode::Track, 1, "LAP", 10000},
            {Mode::Track, 0, "RACE", 10000},
        };
        for (const Want& w : wants) {
            Fixture fx;
            fx.settings.gauge_sweep = false;
            Screen320x480 s;
            fx.app.begin(s.canvas);
            go_to(fx.app, s.canvas, w.m, w.screen);
            Frame f = busy_frame();
            f.lap.phase = core::lap::Phase::Timing;
            fx.app.update(f);
            fx.app.render(s.canvas, true);
            fx.app.clear_full_repaint();
            size_t total = 0;
            const int kFrames = 120;
            for (int i = 0; i < kFrames; ++i) {
                f.t_ms += 17;
                f.lap.current_ms += 17;
                f.motion.best_mph = 30.0f + i * 0.12f;             // a pull
                f.engine.rpm = 5000.0f + i * 30.0f;
                f.att.lean_deg = 20.0f * std::sin(i * 0.05f);       // a corner
                fx.app.update(f);
                App::Damage d[App::kMaxDamage];
                const size_t n = fx.app.damage(d, App::kMaxDamage);
                for (size_t k = 0; k < n; ++k) {
                    const Rect r = d[k].r.clipped_to(Rect{0, 0, kScreenW, kScreenH});
                    total += static_cast<size_t>(r.w) * r.h;
                }
                send_damage(fx.app, s.px);
            }
            const size_t avg = total / kFrames;
            std::printf("       (%s: %zu px a frame)\n", w.name, avg);
            CHECK(std::strcmp(fx.app.current().name(), w.name) == 0);
            CHECK(avg <= w.max_avg_px);
        }
    }

    CASE("an alert stays on top of what changes under it without being resent");
    {
        Fixture fx;
        Screen320x480 partial;
        fx.app.begin(partial.canvas);
        go_to(fx.app, partial.canvas, Mode::Cruise, 0);
        Frame a = busy_frame();
        Frame b = busy_frame();
        b.motion.best_mph = 61.0f;
        b.att.lean_deg = 31.0f;
        b.engine.rpm = 4100.0f;
        fx.app.update(a);
        fx.app.alert().show("TEST ALERT");
        fx.app.render(partial.canvas, true);
        fx.app.clear_full_repaint();

        fx.app.update(b);
        App::Damage d[App::kMaxDamage];
        const size_t n = fx.app.damage(d, App::kMaxDamage);
        bool alert_sent = false, under_alert = false;
        for (size_t i = 0; i < n; ++i) {
            if (d[i].w == &fx.app.alert()) alert_sent = true;
            else if (d[i].r.intersects(fx.app.alert().bounds)) under_alert = true;
        }
        CHECK(under_alert);          // the test is actually testing something
        CHECK(!alert_sent);          // 6.7 ms of wire a frame, when it was
        send_damage(fx.app, partial.px);

        Screen320x480 whole;
        fx.app.render(whole.canvas, true);
        size_t differing = 0;
        for (size_t i = 0; i < partial.px.size(); ++i) {
            if (partial.px[i] != whole.px[i]) ++differing;
        }
        CHECK(differing == 0);
    }

    CASE("an alert that clears puts back only the box it covered, on every screen");
    {
        int screens_tested = 0;
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t sn = 0; sn < counts.screens[m]; ++sn) {
                Fixture fx;
                fx.settings.gauge_sweep = false;
                Screen320x480 partial;
                fx.app.begin(partial.canvas);
                go_to(fx.app, partial.canvas, static_cast<Mode>(m), sn);
                Frame a = busy_frame();
                a.health[Dev::Imu].state = DevState::Fault;     // raises an alert
                for (int i = 0; i < 400 && fx.app.alert_kind() == core::alert::Kind::None; ++i) {
                    a.t_ms += 17;
                    fx.app.update(a);
                }
                if (fx.app.alert_kind() == core::alert::Kind::None) continue;  // e.g. CONFIG
                fx.app.render(partial.canvas, true);
                fx.app.clear_full_repaint();

                Frame b = a;                                     // fault gone
                b.health[Dev::Imu].state = DevState::Ok;
                for (int i = 0; i < 400 && fx.app.alert_kind() != core::alert::Kind::None; ++i) {
                    b.t_ms += 17;
                    fx.app.update(b);
                    if (fx.app.alert_kind() != core::alert::Kind::None) send_damage(fx.app, partial.px);
                }
                CHECK(fx.app.alert_kind() == core::alert::Kind::None);
                CHECK(!fx.app.needs_full_repaint());
                ++screens_tested;
                send_damage(fx.app, partial.px);
                Screen320x480 whole;
                fx.app.render(whole.canvas, true);
                size_t differing = 0;
                for (size_t i = 0; i < partial.px.size(); ++i) {
                    if (partial.px[i] != whole.px[i]) ++differing;
                }
                if (differing) {
                    std::printf("       %s/%s: %zu px differ after the alert cleared\n",
                                fx.app.mode_name(), fx.app.current().name(), differing);
                }
                CHECK(differing == 0);
            }
        }
        std::printf("       (alert cleared on %d screens)\n", screens_tested);
        CHECK(screens_tested >= 10);
    }

    CASE("wheelspin shows as a bar across the top, and clearing it repaints");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 partial;
        fx.app.begin(partial.canvas);
        go_to(fx.app, partial.canvas, Mode::Cruise, 0);
        Frame a = busy_frame();
        a.fused.speed_valid = true;
        a.fused.speed_mph = 20.0f;
        a.motion.front_mph = 20.0f;
        a.motion.rear_mph = 20.0f;
        fx.app.update(a);
        fx.app.render(partial.canvas, true);
        fx.app.clear_full_repaint();
        CHECK(!fx.app.slip_bar().active());

        Frame b = a;
        b.t_ms += 17;
        b.motion.rear_mph = 28.0f;                  // 40% spin
        fx.app.update(b);
        CHECK(fx.app.slip_bar().active());
        CHECK(!fx.app.needs_full_repaint());        // shown as a partial update
        send_damage(fx.app, partial.px);
        Screen320x480 whole;
        fx.app.render(whole.canvas, true);
        size_t differing = 0;
        for (size_t i = 0; i < partial.px.size(); ++i) {
            if (partial.px[i] != whole.px[i]) ++differing;
        }
        CHECK(differing == 0);
        CHECK(partial.px[5 * kScreenW + 5] == color::kWarn);    // amber, top left

        Frame c = a;                                 // grip again...
        c.t_ms = b.t_ms + 2000;                      // ...past the hold
        fx.app.update(c);
        CHECK(!fx.app.slip_bar().active());
        // The strip it covered comes back as damage, not as a whole frame:
        // this happens every time a spin ends.
        CHECK(!fx.app.needs_full_repaint());
        {
            App::Damage dm[App::kMaxDamage];
            const size_t nd = fx.app.damage(dm, App::kMaxDamage);
            bool region = false;
            for (size_t i = 0; i < nd; ++i) region |= dm[i].region;
            CHECK(region);
        }
        send_damage(fx.app, partial.px);
        {
            Screen320x480 whole2;
            fx.app.render(whole2.canvas, true);
            size_t diff2 = 0;
            for (size_t i = 0; i < partial.px.size(); ++i) {
                if (partial.px[i] != whole2.px[i]) ++diff2;
            }
            CHECK(diff2 == 0);
            CHECK(partial.px[5 * kScreenW + 5] != color::kWarn);   // the bar is gone
        }

        fx.settings.slip_bar = false;                // and it can be turned off
        Frame d = b;
        d.t_ms = c.t_ms + 17;
        fx.app.update(d);
        CHECK(!fx.app.slip_bar().active());
    }

    CASE("a jump takes the lean gauge's place on the speed screen, then hands it back");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 partial;
        fx.app.begin(partial.canvas);
        go_to(fx.app, partial.canvas, Mode::Cruise, 0);
        LeanGauge* lean = nullptr;
        const WidgetList wl = fx.app.current().widgets();
        for (size_t i = 0; i < wl.count; ++i) {
            if (std::strcmp(fx.app.current().widget_name(i), "lean") == 0) {
                lean = static_cast<LeanGauge*>(wl.items[i]);
            }
        }
        CHECK(lean != nullptr);
        if (lean) {
            using Face = LeanGauge::Face;
            Frame a = busy_frame();                       // riding, no jump yet
            fx.app.update(a);
            fx.app.render(partial.canvas, true);
            fx.app.clear_full_repaint();
            CHECK(lean->face() == Face::Lean);

            // Every face change goes out as a partial update and lands the
            // same pixels as a full repaint.
            auto same_as_whole = [&]() {
                send_damage(fx.app, partial.px);
                Screen320x480 whole;
                fx.app.render(whole.canvas, true);
                size_t differing = 0;
                for (size_t i = 0; i < partial.px.size(); ++i) {
                    if (partial.px[i] != whole.px[i]) ++differing;
                }
                return differing == 0;
            };

            Frame air = a;                                 // off the lip
            air.t_ms += 300;
            air.att.airborne = true;
            air.att.airtime_ms = 640;
            fx.app.update(air);
            CHECK(lean->face() == Face::Air);
            CHECK(!fx.app.needs_full_repaint());
            CHECK(same_as_whole());

            Frame air2 = air;                              // a tenth is a redraw,
            air2.t_ms += 17;
            air2.att.airtime_ms = 655;                     // a few ms is not
            fx.app.update(air2);
            CHECK(!lean->dirty);

            Frame down = air;                              // landed
            down.t_ms += 700;
            down.att.airborne = false;
            down.att.airtime_ms = 1240;
            down.att.last_land_g = 4.6f;
            down.att.land_ms = down.t_ms;
            fx.app.update(down);
            CHECK(lean->face() == Face::Landed);
            CHECK(same_as_whole());

            Frame later = down;                            // still up just before...
            later.t_ms = down.att.land_ms + 3999;
            fx.app.update(later);
            CHECK(lean->face() == Face::Landed);
            later.t_ms = down.att.land_ms + 4000;          // ...and gone at 4 s
            fx.app.update(later);
            CHECK(lean->face() == Face::Lean);
            CHECK(same_as_whole());

            Frame dead = air;                              // no IMU, no jump face
            dead.t_ms = later.t_ms + 17;
            dead.health[Dev::Imu].state = DevState::Fault;
            fx.app.update(dead);
            CHECK(lean->face() == Face::Lean);
        }
    }

    CASE("a table sends only the rows that changed");
    {
        Table t;
        t.set_bounds(Rect{14, 40, 452, 264});
        t.configure(10, 2);
        for (size_t i = 0; i < 10; ++i) t.set_row(i, "row", "1");
        t.clear_dirty();
        t.set_row(3, "row", "2");
        Rect r[6];
        CHECK(t.damage(r, 6) == 1);
        CHECK(r[0].w == 452);
        CHECK(r[0].h < 40);                          // one row, not 264
        t.set_row(4, "row", "2");                    // adjacent: merged
        CHECK(t.damage(r, 6) == 1);
        t.set_row(8, "row", "2");
        CHECK(t.damage(r, 6) == 2);
        t.clear_dirty();
        t.set_focus(2);                              // highlight moves in
        CHECK(t.damage(r, 6) == 1);
        CHECK(r[0].y >= 40);                         // never above the bounds
        t.clear_dirty();
        t.clear_rows();
        t.clear_dirty();
        t.clear_rows();                              // already empty
        CHECK(!t.dirty);
    }

    CASE("an empty lap history is not resent every frame");
    {
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Track, 2);
        CHECK(std::strcmp(fx.app.current().name(), "LAPS") == 0);
        const Frame f = busy_frame();
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        fx.app.clear_full_repaint();
        fx.app.update(f);
        App::Damage d[App::kMaxDamage];
        CHECK(fx.app.damage(d, App::kMaxDamage) == 0);
    }

    CASE("an arc across north is the same however its angles are written");
    {
        // The lean marker is built as 360 + lean for a left lean, so a
        // marker near upright runs from just under 360 to just over it. The
        // old wrap test dropped everything past north: half the marker.
        Screen320x480 a, b;
        a.canvas.fill_arc(240, 200, 121, 92, -7.0f, 7.0f, 0x1234);
        b.canvas.fill_arc(240, 200, 121, 92, 353.0f, 367.0f, 0x1234);
        size_t ink = 0, differing = 0, left = 0, right = 0;
        for (size_t i = 0; i < a.px.size(); ++i) {
            if (a.px[i] != b.px[i]) ++differing;
            if (b.px[i] == 0x1234) {
                ++ink;
                const int x = static_cast<int>(i % kScreenW);
                if (x < 240) ++left;
                if (x > 240) ++right;            // the north column is neither
            }
        }
        CHECK(ink > 500);
        CHECK(differing == 0);
        CHECK(left == right);                            // both halves drawn
    }

    CASE("an arc drawn into a band is the same rows of the whole arc");
    {
        Screen320x480 whole;
        whole.canvas.fill_arc(240, 200, 116, 97, 270.0f, 360.0f, 0x0F0F);
        whole.canvas.fill_arc(240, 200, 116, 97, 0.0f, 90.0f, 0x0F0F);
        std::vector<Color> band(static_cast<size_t>(kScreenW) * 40, 0);
        FrameBufferCanvas bc(band.data(), kScreenW, 40);
        size_t differing = 0;
        for (int16_t y = 0; y < kScreenH; y += 40) {
            std::fill(band.begin(), band.end(), 0);
            bc.set_window(0, y, kScreenW, 40);
            bc.fill_arc(240, 200, 116, 97, 270.0f, 360.0f, 0x0F0F);
            bc.fill_arc(240, 200, 116, 97, 0.0f, 90.0f, 0x0F0F);
            for (size_t i = 0; i < band.size(); ++i) {
                if (band[i] != whole.px[static_cast<size_t>(y) * kScreenW + i]) ++differing;
            }
        }
        CHECK(differing == 0);
    }

    CASE("a repaint in 40-line bands is the same as one whole-screen repaint");
    {
        // The render task repaints band by band and skips the widgets a
        // band cannot show. Anything drawing outside its own bounds would be
        // lost from the bands that skipped it.
        const Frame f = busy_frame();
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                Fixture fx;
                Screen320x480 whole;
                fx.app.begin(whole.canvas);
                go_to(fx.app, whole.canvas, static_cast<Mode>(m), n);
                fx.app.update(f);
                fx.app.alert().show("OVER IT ALL");
                fx.app.render(whole.canvas, true);

                std::vector<Color> banded(whole.px.size(), 0);
                std::vector<Color> band(static_cast<size_t>(kScreenW) * 40, 0);
                FrameBufferCanvas bc(band.data(), kScreenW, 40);
                for (int16_t y = 0; y < kScreenH; y += 40) {
                    bc.set_window(0, y, kScreenW, 40);
                    fx.app.render(bc, true);
                    std::copy(band.begin(), band.end(),
                              banded.begin() + static_cast<size_t>(y) * kScreenW);
                }
                size_t differing = 0;
                for (size_t i = 0; i < banded.size(); ++i) {
                    if (banded[i] != whole.px[i]) ++differing;
                }
                if (differing != 0) {
                    std::printf("       %s/%s: %zu px differ\n", fx.app.mode_name(),
                                fx.app.current().name(), differing);
                }
                CHECK(differing == 0);
            }
        }
    }

    CASE("the App holds one screen at a time, not all of them");
    {
        // 21 screens held at once were 12.9 kB of the render task's stack,
        // and the demo board's overflowed when they grew. The App is now the
        // largest screen plus the overlays and bookkeeping.
        CHECK(AllScreens::size >= sizeof(SpeedScreen));
        CHECK(sizeof(App) < AllScreens::size + 1024);
    }

    CASE("leaving a screen and coming back draws the same picture");
    {
        // Each screen is destroyed when it is left and built again when it
        // is entered, so whatever the App hands it -- the traces, the lap
        // history, the settings -- has to be handed over again, every time.
        struct Trace final : IPointSource {
            size_t count() const override { return 40; }
            bool point(size_t i, float& x, float& y) const override {
                x = 30.0f * static_cast<float>(i);
                y = 400.0f * std::sin(static_cast<float>(i) * 0.2f);
                return true;
            }
        } trace;
        const uint32_t laps[] = {92500, 91250, 90880, 91900};
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        fx.app.set_trip_source(&trace);
        fx.app.set_track_source(&trace);
        fx.app.set_lap_history(laps, 4, 90880);
        const Frame f = busy_frame();
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                go_to(fx.app, s.canvas, static_cast<Mode>(m), n);
                fx.app.update(f);
                fx.app.render(s.canvas, true);
                const std::vector<Color> first = s.px;
                const ScreenId id = fx.app.current_id();

                // Away to another mode, which destroys it, and back.
                const Mode away = m == 0 ? Mode::Diag : Mode::Cruise;
                go_to(fx.app, s.canvas, away, 0);
                CHECK(fx.app.current_id() != id);
                go_to(fx.app, s.canvas, static_cast<Mode>(m), n);
                CHECK(fx.app.current_id() == id);
                fx.app.update(f);
                fx.app.render(s.canvas, true);

                size_t differing = 0;
                for (size_t i = 0; i < s.px.size(); ++i) if (s.px[i] != first[i]) ++differing;
                if (differing != 0) {
                    std::printf("       %s/%s: %zu px differ after a revisit\n",
                                fx.app.mode_name(), fx.app.current().name(), differing);
                }
                CHECK(differing == 0);
            }
        }
    }

    CASE("a rebuilt map screen still draws the trace the App was handed");
    {
        // Both visits above are built after the wiring, so a screen that
        // never got its trace would match itself. This compares against no
        // trace at all, on the first visit and after a rebuild.
        struct Trace final : IPointSource {
            size_t count() const override { return 40; }
            bool point(size_t i, float& x, float& y) const override {
                x = 30.0f * static_cast<float>(i);
                y = 400.0f * std::sin(static_cast<float>(i) * 0.2f);
                return true;
            }
        } trace;
        const Frame f = busy_frame();
        struct Where { Mode m; uint8_t n; };
        for (const Where w : {Where{Mode::Cruise, 3}, Where{Mode::Track, 3}}) {
            Fixture bare;
            Screen320x480 sb;
            bare.app.begin(sb.canvas);
            go_to(bare.app, sb.canvas, w.m, w.n);
            bare.app.update(f);
            bare.app.render(sb.canvas, true);

            Fixture fx;
            Screen320x480 s;
            fx.app.begin(s.canvas);
            fx.app.set_trip_source(&trace);
            fx.app.set_track_source(&trace);
            for (int visit = 0; visit < 2; ++visit) {
                go_to(fx.app, s.canvas, w.m, w.n);
                fx.app.update(f);
                fx.app.render(s.canvas, true);
                // ink() would not do: the map's panel is lit either way, and
                // a trace only changes the colour of pixels already counted.
                size_t differing = 0;
                for (size_t i = 0; i < s.px.size(); ++i) if (s.px[i] != sb.px[i]) ++differing;
                if (differing < 100) {
                    std::printf("       %s, visit %d: no trace drawn (%zu px differ)\n",
                                fx.app.current().name(), visit + 1, differing);
                }
                CHECK(differing >= 100);
                go_to(fx.app, s.canvas, Mode::Diag, 0);   // destroys it
            }
        }
    }

    CASE("lap history that changes while LAPS is up reaches the screen");
    {
        const uint32_t laps[] = {92500, 91250, 90880, 91900, 90100};
        Fixture fx;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        // Handed over before LAPS exists: the App gives it to the screen
        // when it builds it. Against a LAPS that was never given any.
        fx.app.set_lap_history(laps, 2, 91250);
        go_to(fx.app, s.canvas, Mode::Track, 2);
        CHECK(fx.app.current_id() == ScreenId::LapHistory);
        const Frame f = busy_frame();
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        {
            Fixture bare;
            Screen320x480 sb;
            bare.app.begin(sb.canvas);
            go_to(bare.app, sb.canvas, Mode::Track, 2);
            bare.app.update(f);
            bare.app.render(sb.canvas, true);
            size_t differing = 0;
            for (size_t i = 0; i < s.px.size(); ++i) if (s.px[i] != sb.px[i]) ++differing;
            CHECK(differing >= 100);
        }
        const size_t two = s.ink();
        fx.app.set_lap_history(laps, 5, 90100);   // a lap scored while watching
        fx.app.update(f);
        fx.app.render(s.canvas, true);
        CHECK(s.ink() > two);
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

    CASE("a reading stamped just ahead of the frame's clock repaints nothing");
    {
        // The frame's clock is read before its slots, so a reading that lands
        // in between is a few ms *ahead* of t_ms. An unsigned age made that
        // four billion ms old, and STAT -> HEART blinked to "--" on every
        // reading with the same bpm.
        const core::nav::Layout counts;
        for (uint8_t m = 0; m < core::nav::kModeCount; ++m) {
            for (uint8_t n = 0; n < counts.screens[m]; ++n) {
                Fixture fx;
                fx.settings.gauge_sweep = false;
                fx.settings.hr_on = true;
                Screen320x480 s;
                fx.app.begin(s.canvas);
                go_to(fx.app, s.canvas, static_cast<Mode>(m), n);
                Frame f = busy_frame();
                f.heart.link = core::state::Heart::Live;
                f.heart.bpm = 132; f.heart.at_ms = f.t_ms; f.heart.seq = 5;
                f.heart.avg_bpm = 120; f.heart.max_bpm = 150; f.heart.min_bpm = 90;
                f.heart.readings = 300; f.heart.zone_max_bpm = 185;
                f.heart.contact_known = true; f.heart.contact = true; f.heart.rssi_dbm = -60;
                std::snprintf(f.heart.name, sizeof f.heart.name, "D2 Air X10");
                f.lap.last_done_ms = f.t_ms - 60000; f.lap.split_at_ms = f.t_ms - 60000;
                f.lap.split_ms = 29880; f.lap.last_was_best = true;
                std::snprintf(f.media.title, sizeof f.media.title, "Song");
                std::snprintf(f.media.artist, sizeof f.media.artist, "Band");
                f.media.state = 2; f.media.updated_ms = f.t_ms - 1000;
                f.media.position_ms = 30000; f.media.position_at_ms = f.t_ms; f.media.duration_ms = 200000;
                fx.app.update(f);
                fx.app.render(s.canvas, true);
                fx.app.clear_full_repaint();
                send_damage(fx.app, s.px);
                for (int i = 0; i < 60; ++i) {
                    f.t_ms += 17;
                    if (i % 10 == 3) {          // the same bpm and track again, a few ms ahead
                        f.heart.at_ms = f.t_ms + 3; ++f.heart.seq;
                        f.media.position_at_ms = f.t_ms + 3;
                        f.media.updated_ms = f.t_ms + 3;
                    }
                    fx.app.update(f);
                    App::Damage d[App::kMaxDamage];
                    const size_t k = fx.app.damage(d, App::kMaxDamage);
                    size_t px = 0;
                    for (size_t j = 0; j < k; ++j) px += static_cast<size_t>(d[j].r.w) * d[j].r.h;
                    // ENGINE's rpm trace steps every 100 ms whatever the rpm.
                    const bool trace = std::strcmp(fx.app.current().name(), "ENGINE") == 0;
                    if (px && !trace) {
                        std::printf("       %s/%s repainted %zu px at frame %d\n",
                                    fx.app.mode_name(), fx.app.current().name(), px, i);
                    }
                    CHECK(px == 0 || trace);
                    send_damage(fx.app, s.px);
                }
            }
        }
    }

    CASE("SPEED and RACE carry the watch's heart rate, and no space for it when off");
    {
        struct W { Mode m; uint8_t n; Rect r; };
        const W ws[] = {{Mode::Cruise, 0, Rect{28, 100, 84, 44}},   // under the rpm tile
                        {Mode::Track, 0, Rect{12, 252, 144, 22}}};  // in the gear tile
        for (const W& w : ws) {
            Fixture fx;
            fx.settings.gauge_sweep = false;
            Screen320x480 s;
            fx.app.begin(s.canvas);
            go_to(fx.app, s.canvas, w.m, w.n);
            // `want`: whether that colour is somewhere in it (the edges are
            // anti-aliased blends, the strokes are the colour itself).
            const auto ink_at = [&](const Frame& f, bool* want) {
                fx.app.update(f);
                s.wipe();
                fx.app.render(s.canvas, true);
                size_t n = 0;
                for (int16_t y = w.r.y; y < w.r.bottom(); ++y) {
                    for (int16_t x = w.r.x; x < w.r.right(); ++x) {
                        const Color c = s.px[static_cast<size_t>(y) * kScreenW + x];
                        if (c != color::kBg && c != 0) {
                            ++n;
                            if (want && c == color::kGood) *want = true;
                        }
                    }
                }
                return n;
            };
            Frame f = busy_frame();
            CHECK(ink_at(f, nullptr) == 0);                       // Watch HR off

            f.heart.link = core::state::Heart::Searching;
            const size_t dashes = ink_at(f, nullptr);
            CHECK(dashes > 0);                                     // on, nothing yet: "--"

            f.heart.link = core::state::Heart::Live;
            f.heart.bpm = 142;
            f.heart.at_ms = f.t_ms - 500;
            f.heart.zone_max_bpm = 185;                            // 76 %: zone 3
            bool green = false;
            CHECK(ink_at(f, &green) > dashes);
            CHECK(green);

            f.t_ms += core::hr::kStaleMs;                          // gone quiet
            CHECK(ink_at(f, nullptr) == dashes);
        }
    }

    CASE("HEART graphs the last ten minutes, and repaints it only when a sample lands");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Stat, 5);
        CHECK(std::strcmp(fx.app.current().name(), "HEART") == 0);
        const Rect graph{14, 112, 452, 104};
        const auto ink = [&]() {
            size_t n = 0;
            for (int16_t y = graph.y; y < graph.bottom(); ++y) {
                for (int16_t x = graph.x; x < graph.right(); ++x) {
                    const Color c = s.px[static_cast<size_t>(y) * kScreenW + x];
                    if (c != color::kPanel && c != color::kBg && c != 0) ++n;
                }
            }
            return n;
        };
        Frame f = busy_frame();
        f.t_ms = 1000000;                       // on a 2 s step
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const size_t empty = ink();             // the labels and "no readings yet"

        // Five minutes at 110-150, ridden on another screen.
        go_to(fx.app, s.canvas, Mode::Cruise, 0);
        f.heart.link = core::state::Heart::Live;
        f.heart.worn = true;
        f.heart.zone_max_bpm = 185;
        for (int i = 0; i < 600; ++i) {
            f.t_ms += 500;
            f.heart.bpm = static_cast<uint16_t>(130 + 20 * std::sin(i / 30.0f));
            f.heart.at_ms = f.t_ms;
            fx.app.update(f);
        }
        go_to(fx.app, s.canvas, Mode::Stat, 5);
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(ink() > empty + 300);             // the line is there, kept off-screen
        fx.app.clear_full_repaint();
        send_damage(fx.app, s.px);

        // Within the step, a reading that changes only the number: the graph
        // is not resent. Across it, the graph is.
        const auto graph_damaged = [&]() {
            App::Damage d[App::kMaxDamage];
            const size_t n = fx.app.damage(d, App::kMaxDamage);
            bool hit = false;
            for (size_t k = 0; k < n; ++k) hit = hit || d[k].r.intersects(Rect{14, 140, 452, 50});
            send_damage(fx.app, s.px);
            return hit;
        };
        f.t_ms += 100;
        f.heart.bpm = 141;
        f.heart.at_ms = f.t_ms;
        fx.app.update(f);
        CHECK(!graph_damaged());
        f.t_ms += 2000;
        f.heart.at_ms = f.t_ms;
        fx.app.update(f);
        CHECK(graph_damaged());
    }

    CASE("parked recovery: HEART's tile counts it live, and the result toasts elsewhere");
    {
        Fixture fx;
        fx.settings.gauge_sweep = false;
        Screen320x480 s;
        fx.app.begin(s.canvas);
        go_to(fx.app, s.canvas, Mode::Stat, 5);
        Frame f = busy_frame();
        f.heart.link = core::state::Heart::Live;
        f.heart.bpm = 140;
        f.heart.at_ms = f.t_ms;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        const Rect tile{356 + 4, 226 + 26, 102, 21};
        const auto ink = [&]() {
            size_t n = 0;
            for (int16_t y = tile.y; y < tile.bottom(); ++y)
                for (int16_t x = tile.x; x < tile.right(); ++x) {
                    const Color c = s.px[static_cast<size_t>(y) * kScreenW + x];
                    if (c != color::kBg && c != 0) ++n;
                }
            return n;
        };
        const size_t none = ink();               // "--"

        f.hr_recovery.measuring = true;
        f.hr_recovery.start_ms = f.t_ms - 42000;
        f.hr_recovery.start_bpm = 158;
        f.hr_recovery.live_drop = 18;
        fx.app.update(f);
        s.wipe();
        fx.app.render(s.canvas, true);
        CHECK(ink() > none);                     // "-18"

        // The result, seen from SPEED: a toast.
        go_to(fx.app, s.canvas, Mode::Cruise, 0);
        f.hr_recovery.measuring = false;
        f.hr_recovery.drop1 = 31;
        f.hr_recovery.from_bpm = 158;
        f.hr_recovery.count = 1;
        f.hr_recovery.seq = 1;
        fx.app.update(f);
        CHECK(fx.app.toast().active());
    }

    return check::report("ui");
}
