#include "demo_render.hpp"
#include "esp_log.h"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

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

void render_task_impl(void* arg) {
    auto* pair = static_cast<std::pair<services::Deps*, services::CommandQueue*>*>(arg);
    services::Deps& d = *pair->first;
    services::CommandQueue& q = *pair->second;

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
    // "nobody's measured this." Same fix as health.publish() above: this
    // is the one place that already loops continuously. Mirrors exactly
    // what keeper_service.cpp and render_service.cpp do for the real
    // dash; log/GPS/ctrl/pod/IMU counters stay at zero here on purpose —
    // this demo has none of those things to count.
    uint32_t frames = 0;
    uint32_t last_hz_ms = d.clock->now_ms();

    for (;;) {
        const uint32_t now = d.clock->now_ms();

        core::nav::InputEvent ev;
        if (services::wait_input_event(ev, kPollMs)) {
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
        app.render(canvas, true);
        app.clear_full_repaint();
        if (d.system && d.system->first_frame_ms == 0) {
            d.system->first_frame_ms = now ? now : 1;
            ESP_LOGI("render", "first frame at %u ms", static_cast<unsigned>(now));
        }

        hal::Status s = hal::Status::Ok;
        if (d.panel) {
            s = d.panel->blit(0, 0, ui::kScreenW, ui::kScreenH, buf);
        }
        if (hal::ok(s)) {
            d.health->report_ok(core::state::Dev::Display, now);
        } else {
            d.health->report_error(core::state::Dev::Display,
                                   static_cast<int32_t>(s), now);
        }
        // main.cpp only publishes the tracker's snapshot into the slot once,
        // at boot — nothing has re-published it since, which left every
        // report_ok/report_error above (and anything ble_media.cpp reports)
        // invisible to the Frame every screen actually reads. This is the
        // one place in demo_p4 that already loops continuously and already
        // touches health, so it's the natural place to keep it current.
        d.slots->health.publish(d.health->publish());

        if (d.system) {
            d.system->uptime_s = now / 1000;
            d.system->heap_free_b = static_cast<uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            d.system->heap_min_b = static_cast<uint32_t>(
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            d.system->psram_free_b = static_cast<uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

std::pair<services::Deps*, services::CommandQueue*> g_render_arg;

}  // namespace

void start_demo_render(services::Deps& d, services::CommandQueue& q) {
    g_render_arg = {&d, &q};
    // Larger than the real render task's 8192: that one draws in 40-line
    // bands, this one draws the whole 480x320 screen in one call every
    // tick (see demo_render.hpp for why), and a full repaint through every
    // widget on the busiest screen overflowed 8192 on first boot.
    xTaskCreatePinnedToCore(render_task_impl, "render", 16384, &g_render_arg,
                            6, nullptr, 0);
}

}  // namespace demo
