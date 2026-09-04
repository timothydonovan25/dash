#include "demo_render.hpp"
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

    // The whole screen, one buffer, one shot — see demo_render.hpp for why
    // this doesn't need the real render task's banding.
    auto* buf = static_cast<ui::Color*>(
        heap_caps_malloc(static_cast<size_t>(ui::kScreenW) * ui::kScreenH *
                         sizeof(ui::Color),
                         MALLOC_CAP_8BIT));
    if (!buf) {
        d.health->report_error(core::state::Dev::Display, -1, d.clock->now_ms());
        vTaskDelete(nullptr);
        return;
    }

    ui::FrameBufferCanvas canvas(buf, ui::kScreenW, ui::kScreenH);
    app.begin(canvas);
    if (d.panel) d.panel->set_backlight(d.settings->brightness_pct);

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

        const core::state::Frame frame = core::state::snapshot(*d.slots, now);
        app.update(frame);
        app.render(canvas, true);
        app.clear_full_repaint();

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
