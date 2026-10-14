// What each screen costs on the wire, per frame.
//
// The panel is this dashboard's budget: 307 kB a frame of RGB565 is about
// 31 ms on the P4 board's 8-bit 8080 bus at 10 MHz, two 60 Hz periods, so a
// screen is affordable or not depending on how many pixels it actually
// sends. This runs the firmware's own App, screens and
// widgets against real Frames, asks the widgets themselves what went dirty,
// and prices the result with ui::wire.
//
// No hardware, no ESP-IDF, and no second copy of the render loop: the strip
// split comes from ui/blit_plan.hpp, which is the same function the render
// task blits through.
#include "core/shift/shift.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/wire_cost.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ui;
using core::state::Frame;

namespace {

// Mirrors render_service.cpp: a 40-line band, 480 wide.
constexpr int16_t kBandLines  = 40;
constexpr size_t  kBandPixels = static_cast<size_t>(kScreenW) * kBandLines;

// The bus as bsp/buses.cpp configures it on the P4 board, so a transfer
// the 8080 driver would reject shows up here as a rejected transfer rather
// than as a slow one. 10 MHz is bsp::kTftWrHz.
constexpr uint32_t kBusLimitBytes = 480u * 40u * 2u;

wire::Panel panel_model() {
    wire::Panel p = wire::i80_8bit(10000000u);
    p.bus_limit_bytes = kBusLimitBytes;
    return p;
}

class NullSink final : public ICommandSink {
public:
    bool post(const core::state::Command&) override { return true; }
};

// Navigate the way a rider does, because that is the only way in: A steps
// the mode, B steps the screen. Copied in spirit from test/test_ui.cpp.
void go_to(App& app, Canvas& c, core::nav::Mode m, uint8_t screen) {
    using core::nav::Button;
    using core::nav::InputEvent;
    using core::nav::Press;
    for (int i = 0; i < 8 && app.nav_state().mode != m; ++i) {
        app.on_input(InputEvent{Button::A, Press::Short}, c);
    }
    for (int i = 0; i < 8 && app.nav_state().screen != screen; ++i) {
        app.on_input(InputEvent{Button::B, Press::Short}, c);
    }
}

// A bike at speed, mid-lap, everything healthy. The baseline frame.
Frame riding_frame(uint32_t t_ms) {
    Frame f;
    f.t_ms = t_ms;
    f.engine.rpm = 7250.0f;
    f.engine.coolant_c = 88.0f;
    f.engine.tps_pct = 62.0f;
    f.engine.gear = 4;
    f.engine.shift_mask = 0x07;
    f.motion.gps_mph = 54.0f;
    f.motion.front_mph = 53.6f;
    f.motion.rear_mph = 54.4f;
    f.motion.best_mph = 54.0f;
    f.att.lean_deg = -32.5f;
    f.att.pitch_deg = 3.0f;
    f.att.g_lat = 0.62f;
    f.att.g_lon = 0.10f;
    f.att.g_vert = 1.18f;
    f.gnss.fix = true;
    f.gnss.sats = 11;
    f.gnss.lat = 42.5012;
    f.gnss.lon = -83.1234;
    f.gnss.hdop = 0.8f;
    f.gnss.speed_mph = 54.0f;
    f.gnss.utc_ms = (18 * 3600 + 42 * 60 + 5) * 1000u;
    f.trip.odo_mi = 4821.6f;
    f.trip.trip_a_mi = 31.4f;
    f.trip.eng_hours = 143.2f;
    f.lap.phase = core::lap::Phase::Timing;
    f.lap.count = 4;
    f.lap.current_ms = 42150;
    f.lap.best_ms = 90880;
    f.lap.delta_ms = -320;
    f.elec.vbat = 12.6f;
    for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
        f.health.dev[i].state = core::state::DevState::Ok;
    }
    f.health[core::state::Dev::Ecu].state = core::state::DevState::Absent;
    return f;
}

// One frame later, on a moving bike. Nothing here is a step change: this is
// what a thirtieth of a second of riding looks like, which is the case the
// budget has to survive.
//
// The rpm sweep matters more than it looks. The tach bar quantises the
// marker to whole steps and only dirties the strip it moves between, so a
// frame set that pins rpm above the shift base would report the bar as free
// when it is not. This drags it up through its whole range and back.
Frame next_frame(const Frame& prev, int n, int of) {
    Frame f = prev;
    f.t_ms += 17;

    const float sweep = static_cast<float>(n % of) / static_cast<float>(of);
    f.engine.rpm = 2500.0f + 4000.0f * sweep;
    f.engine.shift_mask =
        core::shift::pattern(static_cast<uint16_t>(f.engine.rpm),
                             core::shift::Config{}, prev.engine.shift_mask);

    f.motion.gps_mph   = 40.0f + 20.0f * sweep;
    f.motion.best_mph  = f.motion.gps_mph;
    f.motion.front_mph = f.motion.gps_mph - 0.4f;
    f.motion.rear_mph  = f.motion.gps_mph + 0.4f;
    f.att.lean_deg     = -32.5f + 0.7f * n;
    f.att.g_lat        = 0.62f + 0.01f * n;
    f.att.g_lon        = 0.10f + 0.005f * n;
    f.trip.odo_mi      = 4821.6f + 0.01f * n;
    f.lap.current_ms   = prev.lap.current_ms + 17;
    f.gnss.utc_ms      = prev.gnss.utc_ms + 17;
    return f;
}

struct Row {
    const char* name;
    Rect        bounds;
    uint32_t    pixels;
    uint32_t    transfers;
    float       ms;
    int         dirty_frames;
};

void rule() { std::printf("  %s\n", std::string(66, '-').c_str()); }

}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 30;
    const wire::Panel p = panel_model();

    std::printf("\nWire cost model — ILI9488, %u MHz, %u bytes/px\n",
                p.clock_hz / 1000000u, p.bytes_per_px);
    std::printf("  per-transfer overhead %u ns, conversion %u ns/px\n",
                p.overhead_ns, p.convert_ns_per_px);
    std::printf("  band %d lines (%zu px), bus limit %u bytes\n",
                kBandLines, kBandPixels, p.bus_limit_bytes);

    // The ceiling, before any screen is involved.
    {
        wire::Model m(p, kBandPixels, kBandLines);
        m.full_repaint();
        const wire::Cost c = m.end_frame();
        std::printf("\nFull repaint: %u transfers, %u px, %.1f ms → %.1f fps ceiling\n",
                    c.transfers, c.pixels, c.ms(), c.fps());
        if (c.oversized) {
            std::printf("  *** %u of %u transfers exceed the bus limit and will be\n"
                        "      rejected by the SPI driver. See bsp/buses.cpp.\n",
                        c.oversized, c.transfers);
        }
    }

    core::config::Settings settings;
    // Riding, not key-on: the frames below never move the clock on, so with
    // the splash on every screen would be priced as the splash's bar.
    settings.splash = false;
    settings.gauge_sweep = false;
    NullSink sink;
    SystemInfo sys{};
    Context ctx;
    ctx.settings = &settings;
    ctx.commands = &sink;
    ctx.system = &sys;

    std::vector<Color> px(static_cast<size_t>(kScreenW) * kScreenH, 0);
    FrameBufferCanvas canvas(px.data(), kScreenW, kScreenH);

    App app(ctx);
    app.bind_settings(&settings);
    app.begin(canvas);

    // Walk every screen in every mode.
    const core::nav::Layout& layout = app.layout();
    for (uint8_t mode = 0; mode < core::nav::kModeCount; ++mode) {
        for (uint8_t idx = 0; idx < layout.screens[mode]; ++idx) {
            // Get onto the screen, then settle it: the first update after a
            // screen change dirties everything by definition, and that is
            // the full-repaint case already priced above.
            go_to(app, canvas, static_cast<core::nav::Mode>(mode), idx);

            Frame f = riding_frame(1000);
            app.update(f);
            app.render(canvas, true);
            app.clear_full_repaint();

            WidgetList ws = app.current().widgets();
            std::vector<Row> rows;
            rows.reserve(ws.count);
            for (Widget* w : ws) {
                rows.push_back(Row{"", w ? w->bounds : Rect{}, 0, 0, 0.0f, 0});
            }

            wire::Model m(p, kBandPixels, kBandLines);
            uint32_t full_frames = 0;

            for (int n = 1; n <= frames; ++n) {
                f = next_frame(f, n, frames);
                app.update(f);

                if (app.needs_full_repaint()) {
                    m.full_repaint();
                    ++full_frames;
                } else {
                    // Exactly what render_service sends: App::damage(), which
                    // is the changed part of each widget, not its bounds.
                    App::Damage dmg[App::kMaxDamage];
                    const size_t nd = app.damage(dmg, App::kMaxDamage);
                    size_t i = 0;
                    for (Widget* w : ws) {
                        // Price this widget alone, so the per-widget
                        // column is the real split of the frame.
                        wire::Model one(p, kBandPixels, kBandLines);
                        bool any = false;
                        for (size_t k = 0; k < nd; ++k) {
                            if (!w || dmg[k].w != w) continue;
                            one.region(dmg[k].r);
                            any = true;
                        }
                        if (any) {
                            const wire::Cost c = one.end_frame();
                            rows[i].pixels    += c.pixels;
                            rows[i].transfers += c.transfers;
                            rows[i].ms        += c.ms();
                            rows[i].dirty_frames++;
                        }
                        ++i;
                    }
                    for (size_t k = 0; k < nd; ++k) m.region(dmg[k].r);
                }
                app.render(canvas, true);
                app.clear_full_repaint();
                m.end_frame();
            }

            std::printf("\n%s / %s — %d frames of riding\n",
                        app.mode_name(), app.current().name(), frames);
            rule();
            std::printf("  %-14s %11s %6s %8s %8s %7s\n",
                        "widget", "bounds", "dirty", "px/frame", "ms/frame", "share");
            rule();

            // Sort by cost, worst first: the point of the table is which
            // widget to fix, not the order they were declared in.
            std::vector<size_t> order(rows.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return rows[a].ms > rows[b].ms;
            });

            const float total_ms = m.mean_ms() * frames;
            for (size_t k : order) {
                const Row& r = rows[k];
                if (r.dirty_frames == 0) continue;
                char bnd[16];
                std::snprintf(bnd, sizeof bnd, "%dx%d", r.bounds.w, r.bounds.h);
                char fallback[8];
                const char* wname = app.current().widget_name(k);
                if (!wname) {
                    std::snprintf(fallback, sizeof fallback, "w%zu", k);
                    wname = fallback;
                }
                std::printf("  %-14s %11s %5d%% %8.0f %8.3f %6.1f%%\n",
                            wname, bnd,
                            100 * r.dirty_frames / frames,
                            static_cast<double>(r.pixels) / frames,
                            static_cast<double>(r.ms) / frames,
                            total_ms > 0 ? 100.0 * r.ms / total_ms : 0.0);
            }
            rule();
            std::printf("  %-14s %11s %6s %8.0f %8.3f\n", "TOTAL", "", "",
                        static_cast<double>(m.total().pixels) / frames,
                        static_cast<double>(m.mean_ms()));
            if (m.mean_ms() > 0.0f) {
                std::printf("\n  mean %.2f ms/frame → %.0f fps sustainable"
                            "   (60 Hz needs 16.67 ms)\n",
                            m.mean_ms(), 1000.0f / m.mean_ms());
            } else {
                std::printf("\n  nothing moved: this screen costs no wire time "
                            "between full repaints\n");
            }
            std::printf("  peak %.2f ms   full repaints %u   unsendable %u\n",
                        m.peak().ms(), full_frames, m.unsendable());
            if (m.total().oversized) {
                std::printf("  *** %u transfers over the bus limit\n",
                            m.total().oversized);
            }
        }
    }
    std::printf("\n");
    return 0;
}
