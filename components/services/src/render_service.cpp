// `render` — core 1, priority 6, 120 Hz.
//
// One snapshot of every slot, one update() per screen, one repaint of
// whatever went dirty. The screen never reads a slot itself (rule 4), which
// is what makes the same App run in a host test against a literal Frame.
//
// Drawing happens in bands, and — this is the part that decides the frame
// rate — only the parts of the screen that changed are sent to the panel.
//
// On the P4 board the ILI9488 is on an 8-bit 8080 bus: a full 480x320 frame
// is 307 kB of RGB565, about 31 ms at a 10 MHz write strobe — four 120 Hz
// periods. The widget dirty flags are what make a high rate possible, and the
// widgets' own damage() narrows them to the part that changed: the lean
// marker and its number, the digits of a speed or a lap time that moved.
// sim/wire_report prices a riding frame on SPEED at about 2 ms (5.6 peak);
// it was 6.6 (10 peak) while those went whole. A full repaint simply takes
// several periods, and the deadline below skips ahead rather than trying to
// catch up. On the S3 board's SPI the same frame was 92 ms, and the dirty
// flags were the difference between 11 fps and 30.
//
// A whole frame still goes out on a screen change and at boot — anything
// where the panel's contents are unknown. An alert being dismissed or the
// slip bar clearing is not that: only the box it covered goes again.
#include "services/services.hpp"
#include "ui/app.hpp"
#include "ui/blit_plan.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "bsp/pins.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

namespace services {
namespace {

constexpr const char* kTag = "render";

// Frames are paced to a 120 Hz deadline, about twice the ILI9488's own
// refresh rate. Writing faster than the panel scans does not put a number on
// the glass sooner than its next scan, but it halves the time a changed
// number waits for *this* loop before going out: up to 8.3 ms instead of
// 16.7. It costs CPU, not bus: a pass that finds nothing dirty sends nothing
// and costs one walk of the flags.
//
// A button press cuts the wait short and is drawn at once, so the pacing
// bounds how stale a *number* can be, not how fast a menu responds.
//
// The deadline is kept in whole frames from a fixed start, so the 8.33 ms
// period does not drift by rounding to the 1 ms tick.
constexpr uint32_t kFrameHz = 120;

constexpr int16_t kBandLines = bsp::kTftBandLines;
constexpr size_t  kBandPixels = static_cast<size_t>(ui::kScreenW) * kBandLines;

// Both traces, exposed to the map widgets as a read-only view. The widget
// walks the buffer; it never gets a copy of it (§4.4).
class TraceSource final : public ui::IPointSource {
public:
    using Buffer = core::state::TrackBuffer<core::state::TrackPoint, 4096>;
    explicit TraceSource(Buffer* b) : buf_(b) {}

    size_t count() const override { return buf_ ? buf_->size() : 0; }
    size_t total() const override { return buf_ ? buf_->total() : 0; }
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

// Push one damaged rectangle, in horizontal strips that fit the one band
// buffer. A tall widget becomes a few transfers rather than needing a
// framebuffer the size of the screen.
hal::Status blit_damage(Deps& d, ui::App& app, ui::FrameBufferCanvas& canvas,
                        ui::Color* buf, const ui::App::Damage& dmg) {
    // The clip and the strip split are in ui/blit_plan.hpp so that the
    // wire-cost model prices exactly what this sends, rather than its own
    // idea of it.
    ui::Rect strips[ui::kMaxStrips];
    const int n = ui::plan_strips(dmg.r, kBandPixels, strips, ui::kMaxStrips);
    if (n == 0) return hal::Status::Ok;
    if (n < 0)  return hal::Status::Unsupported;

    hal::Status s = hal::Status::Ok;
    for (int i = 0; i < n; ++i) {
        const ui::Rect& r = strips[i];
        if (!canvas.set_window(r.x, r.y, r.w, r.h)) return hal::Status::Unsupported;
        app.draw_damage(canvas, dmg);
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

    // On this task's stack, which start_render sizes for it: App is about
    // 1.9 kB, the one live screen plus the overlays (it held all 21 screens,
    // 13 kB, until 1.17). Not a static: .bss comes out of the internal
    // RAM the boot itself runs in, which ESP-Hosted's buffers already nearly
    // fill, and 11 kB more there left no room to create the idle tasks. A
    // task stack is allocated after the scheduler starts, from the larger
    // region IDF only adds then. Internal either way: every frame touches it.
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



    // The backlight goes up with the first frame. `ctrl` sends it, as it
    // does every LED-driver write; `keeper` takes over from its next pass.
    d.backlight_pct.store(d.settings->brightness_pct, std::memory_order_relaxed);

    uint32_t frames = 0;
    uint32_t last_hz_ms = d.clock->now_ms();
    bool last_send_ok = true;

    uint32_t start_ms = d.clock->now_ms();
    uint32_t frame_no = 1;
    auto deadline_ms = [&] { return start_ms + frame_no * 1000u / kFrameHz; };

    for (;;) {
        // Wait here rather than at the bottom of the loop, so a button press
        // is acted on the moment it arrives instead of after the rest of the
        // period has run out. That is the difference between a menu that
        // feels immediate and one that feels like it is thinking about it.
        const uint32_t before = d.clock->now_ms();
        const int32_t left = static_cast<int32_t>(deadline_ms() - before);
        core::nav::InputEvent ev;
        if (left > 0 && wait_input_event(ev, static_cast<uint32_t>(left))) {
            app.on_input(ev, canvas);
            while (take_input_event(ev)) app.on_input(ev, canvas);
        } else {
            // The period ran out: one frame. A pass woken early by a button
            // is not one, and a deadline skipped below is one missed, so
            // DIAG's rate reads 60 when every frame takes two periods.
            ++frames;
            // The period ran out. Advance to the next deadline still in the
            // future: after a full repaint that is several frames on, not a burst
            // of catch-up frames that would each find nothing to send.
            const uint32_t t = d.clock->now_ms();
            while (static_cast<int32_t>(deadline_ms() - t) <= 0) {
                // Rebase once a second so frame_no * 1000 never overflows.
                if (++frame_no > kFrameHz) {
                    frame_no -= kFrameHz;
                    start_ms += 1000;
                }
            }
        }
        const uint32_t now = d.clock->now_ms();

        // A re-init `keeper` asked for after the panel faulted. Here, between
        // frames: from keeper it reset the panel under a transfer. Whatever
        // was on the glass is gone, so the next frame goes out whole.
        if (take_retry(d, core::state::Dev::Display) && d.panel) {
            d.panel->init();
            app.invalidate();
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
        bool sent = false;

        if (app.needs_full_repaint()) {
            sent = true;
            // The panel's contents are unknown, so everything goes out: the
            // background chrome as well as every widget, band by band.
            for (int16_t y = 0; y < ui::kScreenH; y += kBandLines) {
                canvas.set_window(0, y, ui::kScreenW, kBandLines);
                app.render(canvas, true);
                if (d.panel && hal::ok(blit_status)) {
                    blit_status = d.panel->blit(0, static_cast<uint16_t>(y),
                                                ui::kScreenW, kBandLines, band);
                }
            }
            // A band that did not go out leaves the panel unknown: repaint
            // again next frame rather than call it done.
            if (hal::ok(blit_status)) app.clear_full_repaint();
            if (d.system && d.system->first_frame_ms == 0) {
                d.system->first_frame_ms = now ? now : 1;
                // The boot-time number: switch to a picture, less the rails.
                ESP_LOGI(kTag, "first frame at %u ms", static_cast<unsigned>(now));
            }
        } else {
            // Only what moved, and only the part of it that moved. Each
            // widget clears its own bounds before drawing, and the
            // background chrome sits outside those bounds, so what is
            // already on the panel underneath stays correct.
            ui::App::Damage dmg[ui::App::kMaxDamage];
            const size_t n = app.damage(dmg, ui::App::kMaxDamage);
            for (size_t i = 0; i < n && hal::ok(blit_status); ++i) {
                blit_status = blit_damage(d, app, canvas, band, dmg[i]);
                sent = true;
            }
            // A failed send leaves it all dirty, to go again next frame.
            if (hal::ok(blit_status)) app.clear_damage();
        }

        // Only a transfer says anything about the panel. Reporting Ok on
        // the frames that sent nothing used to wipe the error streak of a
        // panel that fails only when written to.
        // A still screen sends nothing for seconds, so an idle frame
        // vouches for the panel only while its last transfer worked.
        if (sent) last_send_ok = hal::ok(blit_status);
        if (sent && !last_send_ok) {
            d.health->report_error(core::state::Dev::Display,
                                   static_cast<int32_t>(blit_status), now);
        } else if (last_send_ok) {
            d.health->report_ok(core::state::Dev::Display, now);
        }

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
    // 8 kB for the task itself plus the App that lives on its stack.
    xTaskCreatePinnedToCore(render_task, "render", 8192 + sizeof(ui::App),
                            &g_render_arg, 6, nullptr, 1);
}

}  // namespace services
