#include "demo_render.hpp"
#include "esp_log.h"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
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

void render_task_impl(void* arg) {
    auto* ra = static_cast<RenderArg*>(arg);
    services::Deps& d = *ra->deps;
    services::CommandQueue& q = *ra->commands;
    StateRequest& state_req = *ra->state_req;

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

        if (d.lap_history) {
            app.set_lap_history(d.lap_history->ms, d.lap_history->count,
                                d.lap_history->best_ms);
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
        if (app.needs_full_repaint()) {
            canvas.set_window(0, 0, ui::kScreenW, ui::kScreenH);
            app.render(canvas, true);
            if (d.panel) s = d.panel->blit(0, 0, ui::kScreenW, ui::kScreenH, buf);
            sent = true;
            if (hal::ok(s)) app.clear_full_repaint();
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
                if (d.panel) {
                    s = d.panel->blit(static_cast<uint16_t>(r.x), static_cast<uint16_t>(r.y),
                                      static_cast<uint16_t>(r.w), static_cast<uint16_t>(r.h),
                                      buf);
                }
                sent = true;
            }
            if (hal::ok(s)) app.clear_damage();
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

RenderArg g_render_arg;

}  // namespace

void start_demo_render(services::Deps& d, services::CommandQueue& q,
                       StateRequest& req) {
    g_render_arg = {&d, &q, &req};
    // ui::App lives on this task's stack, so the stack is sized from it, as
    // the real render task's is. A fixed 16384 overflowed in 1.16, when the
    // App still held every screen and grew from 11.4 to 13.2 kB; it holds
    // one screen now, but the size still follows it.
    xTaskCreatePinnedToCore(render_task_impl, "render", 8192 + sizeof(ui::App),
                            &g_render_arg, 6, nullptr, 0);
}

}  // namespace demo
