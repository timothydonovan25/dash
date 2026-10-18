#include "demo_render.hpp"
#include "core/stack.hpp"
#include "esp_log.h"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/shadow.hpp"
#include "ui/wire_cost.hpp"
#include "drivers/chip_temp.hpp"
#include "core/wifi/wifi.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace demo {
namespace {

constexpr uint32_t kPollMs = 20;   // the demo has nowhere near the real
                                   // dash's 3ms shift-light budget to protect

// Same class render_service.cpp uses for the real dash's map screens: a
// read-only view over the trip/lap breadcrumb buffers, walked by the
// widget rather than copied out of (§4.4).
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

struct RenderArg {
    services::Deps*         deps;
    services::CommandQueue* commands;
    StateRequest*           state_req;
    DoomProgram*            doom;
};

// The answer to `state`, as key=value so a script can parse it. Screen and
// focus are 0-based, the way B and D presses count them.
void log_state(ui::App& app, const core::state::Frame& f) {
    const core::nav::State& n = app.nav_state();
    ui::Screen& s = app.current();
    ESP_LOGI("state", "mode=%s screen=%u/%u name=%s focus=%u/%u menu=%d splash=%d "
             "slip=%d wifi=%s clients=%u",
             app.mode_name(), static_cast<unsigned>(n.screen),
             static_cast<unsigned>(app.layout().screens[static_cast<uint8_t>(n.mode)]),
             s.name(), static_cast<unsigned>(n.focus),
             static_cast<unsigned>(s.focus_count()), n.menu_open ? 1 : 0,
             app.splashing() ? 1 : 0, app.slip_bar().active() ? 1 : 0,
             core::wifi::state_name(f.wifi.state),
             static_cast<unsigned>(f.wifi.clients));
}

DASH_BIG_FRAME_BEGIN   // ui::App (~2 kB) is a local; start_demo_render sizes the stack for it
void render_task_impl(void* arg) {
    auto* ra = static_cast<RenderArg*>(arg);
    services::Deps& d = *ra->deps;
    services::CommandQueue& q = *ra->commands;
    StateRequest& state_req = *ra->state_req;

    ui::Context ctx;
    ctx.settings = d.settings;
    ctx.commands = &q;
    ctx.system = d.system;
    ctx.program = ra->doom;

    ui::App app(ctx);
    app.bind_settings(d.settings);

    TraceSource trip_src(d.trip_trace);
    TraceSource lap_src(d.lap_trace);
    app.set_trip_source(&trip_src);
    app.set_track_source(&lap_src);

    // The whole screen, one buffer, one shot — see demo_render.hpp for why
    // this doesn't need the real render task's banding. In PSRAM: it is
    // 300 kB, and asked for as plain MALLOC_CAP_8BIT it came out of internal
    // RAM and left the SDIO link, lwIP and NimBLE about 17 kB between them,
    // which a Wi-Fi upload ran out of. Only the CPU touches it.
    auto* buf = static_cast<ui::Color*>(
        heap_caps_malloc(static_cast<size_t>(ui::kScreenW) * ui::kScreenH *
                         sizeof(ui::Color),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        d.health->report_error(core::state::Dev::Display, -1, d.clock->now_ms());
        vTaskDelete(nullptr);
        return;
    }

    ui::FrameBufferCanvas canvas(buf, ui::kScreenW, ui::kScreenH);
    app.begin(canvas);

    // What the glass shows, so a redrawn widget sends only the pixels that
    // changed (ui::Shadow), as the bike's render task does. Here the saving
    // is CPU and PSRAM writes -- every pixel blit() takes is scaled 1.5x and
    // turned -- rather than bus time, but it is the same code path, and this
    // is the board it can be watched on.
    auto* glass = static_cast<ui::Color*>(
        heap_caps_malloc(static_cast<size_t>(ui::kScreenW) * ui::kScreenH *
                         sizeof(ui::Color), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!glass) ESP_LOGW("render", "no PSRAM for the shadow: sending damage whole");
    ui::Shadow shadow(glass);
    // The bike's bus, priced: the demo has none, and uses the bike's merge
    // rule so the parts it sends are the ones the bike would.
    const uint32_t overhead_px = ui::wire::overhead_px(ui::wire::i80_8bit(10000000u));
    if (d.panel) d.panel->set_backlight(d.settings->brightness_pct);
    uint8_t applied_brightness = d.settings->brightness_pct;

    // DIAG's System screen (ui::SystemInfo) was reading this struct's
    // untouched, default-constructed zeros — heap/PSRAM/uptime/render Hz
    // all "0" forever, which reads as a genuine reading rather than as
    // "nobody's measured this." Filled in below, since this is the one
    // place that already loops continuously. Mirrors exactly
    // what keeper_service.cpp and render_service.cpp do for the real
    // dash; log/GPS/ctrl/pod/IMU counters stay at zero here on purpose —
    // this demo has none of those things to count.
    uint32_t frames = 0;
    uint32_t last_hz_ms = d.clock->now_ms();

    bool last_send_ok = true;
    uint32_t last_stats_ms = 0;
    drivers::ChipTemp chip_temp;
    const bool chip_temp_up = hal::ok(chip_temp.init());
    const ui::Rect screen{0, 0, ui::kScreenW, ui::kScreenH};

    for (;;) {
        const uint32_t now = d.clock->now_ms();

        core::nav::InputEvent ev;
        if (services::wait_input_event(ev, kPollMs)) {
            // A screen change lays the new screen out against the canvas, so
            // give it the whole one, not the last damaged rectangle.
            canvas.set_window(0, 0, ui::kScreenW, ui::kScreenH);
            app.on_input(ev, canvas);
            while (services::take_input_event(ev)) app.on_input(ev, canvas);
        }

        // DIAG -> Doom: the panel is Doom's from here until a reset. Nothing
        // is drawn after this, so nothing of the dash's is left mid-write.
        if (ra->doom && ra->doom->requested()) {
            ESP_LOGI("render", "handing the panel to Doom");
            heap_caps_free(glass);
            heap_caps_free(buf);
            ra->doom->start();
            vTaskDelete(nullptr);
        }

        if (d.lap_history) {
            app.set_lap_history(d.lap_history->ms, d.lap_history->count,
                                d.lap_history->best_ms, d.lap_history->peak);
        }

        // CONFIG -> Display -> Brightness takes effect as it is changed,
        // which the real dash gets from `keeper`; this demo has no keeper.
        if (d.panel && d.settings->brightness_pct != applied_brightness) {
            applied_brightness = d.settings->brightness_pct;
            d.panel->set_backlight(applied_brightness);
        }

        const core::state::Frame frame = core::state::snapshot(*d.slots, now);
        app.update(frame);
        // After update, so a press just taken is already reflected.
        if (state_req.pending.exchange(false)) log_state(app, frame);

        hal::Status s = hal::Status::Ok;
        bool sent = false;
        // `r`, just drawn into buf (packed), sent only where it differs from
        // the glass, each part packed to the front of the buffer and written
        // before the next.
        auto send_drawn = [&](const ui::Rect& r) {
            ui::Rect parts[ui::Shadow::kMaxParts];
            const int np = shadow.diff(r, buf, parts, ui::Shadow::kMaxParts, overhead_px);
            for (int k = 0; k < np && hal::ok(s); ++k) {
                const ui::Rect& p = parts[k];
                ui::Shadow::pack(r, buf, p);
                if (d.panel) {
                    s = d.panel->blit(static_cast<uint16_t>(p.x), static_cast<uint16_t>(p.y),
                                      static_cast<uint16_t>(p.w), static_cast<uint16_t>(p.h),
                                      buf);
                }
                sent = true;
            }
        };

        if (app.needs_full_repaint()) {
            canvas.set_window(0, 0, ui::kScreenW, ui::kScreenH);
            app.render(canvas, true);
            // A screen change over a glass the shadow knows sends only what
            // differs from the screen before; an unknown glass, all of it.
            if (shadow.valid()) {
                send_drawn(screen);
            } else {
                if (d.panel) s = d.panel->blit(0, 0, ui::kScreenW, ui::kScreenH, buf);
                sent = true;
                shadow.record(screen, buf);
            }
            if (hal::ok(s)) {
                app.clear_full_repaint();
                shadow.validate();
            } else {
                shadow.invalidate();
            }
            if (d.system && d.system->first_frame_ms == 0) {
                d.system->first_frame_ms = now ? now : 1;
                ESP_LOGI("render", "first frame at %u ms", static_cast<unsigned>(now));
            }
        } else {
            // Only what changed. Each rectangle is drawn into the one buffer
            // windowed onto just that rectangle, so it comes out packed, the
            // way blit() takes it.
            ui::App::Damage dmg[ui::App::kMaxDamage];
            const size_t n = app.damage(dmg, ui::App::kMaxDamage);
            for (size_t i = 0; i < n && hal::ok(s); ++i) {
                const ui::Rect r = dmg[i].r.clipped_to(screen);
                if (r.empty() || !canvas.set_window(r.x, r.y, r.w, r.h)) continue;
                app.draw_damage(canvas, dmg[i]);
                send_drawn(r);
            }
            // A failed write leaves the glass unknown where the shadow says
            // it was sent: a whole frame next, which puts them back in step.
            if (hal::ok(s)) {
                app.clear_damage();
            } else {
                shadow.invalidate();
                app.invalidate();
            }
        }

        // As on the bike: only a transfer says anything about the panel,
        // and a still screen is vouched for while its last transfer worked.
        if (sent) last_send_ok = hal::ok(s);
        if (sent && !last_send_ok) {
            d.health->report_error(core::state::Dev::Display,
                                   static_cast<int32_t>(s), now);
        } else if (last_send_ok) {
            d.health->report_ok(core::state::Dev::Display, now);
        }

        // The card, as the bike's `storage` and `logger` vouch for theirs:
        // the tracker calls a device that has not reported in 2 s stale and
        // faults it, and a card mounted once at boot and never reported on
        // again put the SD alert ("LOG STOPPED") over the demo. One that
        // did not mount stays absent.
        if (d.card && d.card->present()) {
            d.health->report_ok(core::state::Dev::Sd, now);
        }

        // Keeper's health job, since this demo has no keeper: turn the
        // reports (here, ble_service.cpp and the pod) into states, and
        // publish the table when it changes.
        d.health->tick(now, core::health::Policy{});
        d.health->publish_if_changed(d.slots->health, now);

        // Heap figures once a second, as keeper does: each call takes the
        // heap lock, and DIAG reads them at a glance, not at 50 Hz.
        if (d.system && now - last_stats_ms >= 1000) {
            last_stats_ms = now;
            d.system->uptime_s = now / 1000;
            d.system->heap_free_b = static_cast<uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            d.system->heap_min_b = static_cast<uint32_t>(
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            d.system->psram_free_b = static_cast<uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            float c = 0.0f;
            const bool ok = chip_temp_up && hal::ok(chip_temp.read(c));
            if (ok) d.system->chip_temp_c = c;
            d.system->chip_temp_ok = ok;
            services::watch_stacks(*d.system, now);
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
DASH_BIG_FRAME_END

RenderArg g_render_arg;

}  // namespace

void start_demo_render(services::Deps& d, services::CommandQueue& q,
                       StateRequest& req, DoomProgram* doom) {
    g_render_arg = {&d, &q, &req, doom};
    // ui::App lives on this task's stack, so the stack is sized from it, as
    // the real render task's is. A fixed 16384 overflowed in 1.16, when the
    // App still held every screen and grew from 11.4 to 13.2 kB; it holds
    // one screen now, but the size still follows it.
    xTaskCreatePinnedToCore(render_task_impl, "render", 8192 + sizeof(ui::App),
                            &g_render_arg, 6, nullptr, 0);
}

}  // namespace demo
