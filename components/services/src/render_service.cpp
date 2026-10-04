// `render` — core 1, priority 6, 30 Hz.
//
// One snapshot of every slot, one update() per screen, one repaint of
// whatever went dirty. The screen never reads a slot itself (rule 4), which
// is what makes the same App run in a host test against a literal Frame.
//
// Drawing happens in bands, and — this is the part that decides the frame
// rate — only the parts of the screen that changed are sent to the panel.
//
// On the P4 board the ILI9488 is on an 8-bit 8080 bus: a full 480x320 frame
// is 307 kB of RGB565, about 31 ms at a 10 MHz write strobe. That is just
// inside 30 Hz on its own, but not with anything else on the frame, so the
// widget dirty flags stay until a measured number says otherwise
// (docs/s3-parallel-respin.md §6). On the S3 board's SPI the same frame was
// 92 ms, and the dirty flags were the difference between 11 fps and 30.
//
// A whole frame still goes out on a screen change, when an alert is
// dismissed, and at boot — anything where the panel's contents are unknown.
#include "services/services.hpp"
#include "ui/app.hpp"
#include "ui/blit_plan.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "bsp/pins.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

namespace services {
namespace {

// How long the loop will sit waiting before looking at the state again. A
// button press cuts the wait short, so this only bounds how stale a *number*
// can be, and it is cheap: a pass that finds nothing dirty sends nothing.
//
// It is not the frame rate. Frames go out when something changes, and a
// screen where nothing has changed costs one walk of thirteen dirty flags.
constexpr uint32_t kPollMs = 10;

constexpr int16_t kBandLines = bsp::kTftBandLines;
constexpr size_t  kBandPixels = static_cast<size_t>(ui::kScreenW) * kBandLines;

// Both traces, exposed to the map widgets as a read-only view. The widget
// walks the buffer; it never gets a copy of it (§4.4).
class TraceSource final : public ui::IPointSource {
public:
    using Buffer = core::state::TrackBuffer<core::state::TrackPoint, 4096>;
    explicit TraceSource(Buffer* b) : buf_(b) {}

    size_t count() const override { return buf_ ? buf_->size() : 0; }
    bool point(size_t i, float& x_ft, float& y_ft) const override {
        core::state::TrackPoint p;
        if (!buf_ || !buf_->read_nth(i, p)) return false;
        x_ft = p.x_ft;
        y_ft = p.y_ft;
        return true;
    }

private:
    Buffer* buf_;
};

// Push one widget's rectangle, in horizontal strips that fit the one band
// buffer. A tall widget becomes a few transfers rather than needing a
// framebuffer the size of the screen.
hal::Status blit_widget(Deps& d, ui::FrameBufferCanvas& canvas, ui::Color* buf,
                        ui::Widget& w) {
    // The clip and the strip split are in ui/blit_plan.hpp so that the
    // wire-cost model prices exactly what this sends, rather than its own
    // idea of it.
    ui::Rect strips[ui::kMaxStrips];
    const int n = ui::plan_strips(w.bounds, kBandPixels, strips, ui::kMaxStrips);
    if (n == 0) return hal::Status::Ok;
    if (n < 0)  return hal::Status::Unsupported;

    hal::Status s = hal::Status::Ok;
    for (int i = 0; i < n; ++i) {
        const ui::Rect& r = strips[i];
        if (!canvas.set_window(r.x, r.y, r.w, r.h)) return hal::Status::Unsupported;
        w.render(canvas);
        if (d.panel) {
            s = d.panel->blit(static_cast<uint16_t>(r.x),
                              static_cast<uint16_t>(r.y),
                              static_cast<uint16_t>(r.w),
                              static_cast<uint16_t>(r.h), buf);
            if (!hal::ok(s)) return s;
        }
    }
    return s;
}

void render_task(void* arg) {
    auto* pair = static_cast<std::pair<Deps*, CommandQueue*>*>(arg);
    Deps& d = *pair->first;
    CommandQueue& q = *pair->second;

    ui::Context ctx;
    ctx.settings = d.settings;
    ctx.commands = &q;
    ctx.system = d.system;

    ui::App app(ctx);
    app.bind_settings(d.settings);

    TraceSource trip_src(d.trip_trace);
    TraceSource lap_src(d.lap_trace);
    app.set_trip_source(&trip_src);
    app.set_track_source(&lap_src);

    // Internal SRAM, not PSRAM. This is 38 kB that is redrawn for every
    // transfer; the panel driver copies it into its own DMA staging buffer,
    // so it is the CPU, not the DMA, that reads it. The big buffers that can
    // afford to live in PSRAM are the traces, and they do — see wiring.cpp.
    auto* band = static_cast<ui::Color*>(
        heap_caps_malloc(kBandPixels * sizeof(ui::Color),
                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!band) {
        // Nothing to draw into. Fail visibly rather than silently: the dash
        // is useless without a screen and the rider needs to know why.
        d.health->report_error(core::state::Dev::Display, -1, d.clock->now_ms());
        vTaskDelete(nullptr);
        return;
    }

    ui::FrameBufferCanvas canvas(band, ui::kScreenW, kBandLines);
    app.begin(canvas);



    if (d.panel) d.panel->set_backlight(d.settings->brightness_pct);

    uint32_t frames = 0;
    uint32_t last_hz_ms = d.clock->now_ms();

    for (;;) {
        const uint32_t now = d.clock->now_ms();

        // Wait here rather than at the bottom of the loop, so a button press
        // is acted on the moment it arrives instead of after the rest of the
        // period has run out. That is the difference between a menu that
        // feels immediate and one that feels like it is thinking about it.
        core::nav::InputEvent ev;
        if (wait_input_event(ev, kPollMs)) {
            app.on_input(ev, canvas);
            while (take_input_event(ev)) app.on_input(ev, canvas);
        }

        // The history grows as laps are scored, so the view is handed over
        // every frame rather than once at startup.
        if (d.lap_history) {
            app.set_lap_history(d.lap_history->ms, d.lap_history->count,
                                d.lap_history->best_ms);
        }

        // One coherent read of every slot, once per frame.
        const core::state::Frame frame = core::state::snapshot(*d.slots, now);
        app.update(frame);

        hal::Status blit_status = hal::Status::Ok;

        if (app.needs_full_repaint()) {
            // The panel's contents are unknown, so everything goes out: the
            // background chrome as well as every widget, band by band.
            for (int16_t y = 0; y < ui::kScreenH; y += kBandLines) {
                canvas.set_window(0, y, ui::kScreenW, kBandLines);
                app.render(canvas, true);
                if (d.panel) {
                    blit_status = d.panel->blit(0, static_cast<uint16_t>(y),
                                                ui::kScreenW, kBandLines, band);
                }
            }
            app.clear_full_repaint();
        } else {
            // Only what moved. Each widget clears its own bounds before
            // drawing, and the background chrome sits outside those bounds,
            // so what is already on the panel underneath stays correct.
            for (ui::Widget* w : app.current().widgets()) {
                if (!w || !w->dirty) continue;
                blit_status = blit_widget(d, canvas, band, *w);
                w->clear_dirty();
            }
            // The alert sits over everything, so anything repainted under it
            // has to be covered again.
            if (app.alert().active()) {
                blit_status = blit_widget(d, canvas, band, app.alert());
                app.alert().clear_dirty();
            }
        }

        if (hal::ok(blit_status)) {
            d.health->report_ok(core::state::Dev::Display, now);
        } else {
            d.health->report_error(core::state::Dev::Display,
                                   static_cast<int32_t>(blit_status), now);
        }

        ++frames;
        if (now - last_hz_ms >= 1000) {
            if (d.system) {
                d.system->render_hz_x10 =
                    static_cast<uint16_t>(frames * 10000 / (now - last_hz_ms));
            }
            frames = 0;
            last_hz_ms = now;
        }

    }
}

std::pair<Deps*, CommandQueue*> g_render_arg;

}  // namespace

void start_render(Deps& d, CommandQueue& q) {
    g_render_arg = {&d, &q};
    // Core 1, with the card and the radio. The panel can afford to be late;
    // the shift lights cannot, and they are on the other core.
    xTaskCreatePinnedToCore(render_task, "render", 8192, &g_render_arg, 6,
                            nullptr, 1);
}

}  // namespace services
