// `render` — core 1, priority 6, 30 Hz.
//
// One snapshot of every slot, one update() per screen, one repaint of
// whatever went dirty. The screen never reads a slot itself (rule 4), which
// is what makes the same App run in a host test against a literal Frame.
//
// Drawing happens in bands. A full 320x480 frame at the ILI9488's 18-bit
// colour is 460 kB, which does not fit in DRAM and would be a long DMA even
// if it did (§7.6).
#include "services/services.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "bsp/pins.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

namespace services {
namespace {

constexpr int16_t kBandLines = 40;
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

    // Two band buffers in PSRAM, DMA-capable. One is drawn into while the
    // other is in flight.
    auto* band = static_cast<ui::Color*>(
        heap_caps_malloc(kBandPixels * sizeof(ui::Color),
                         MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
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

        core::nav::InputEvent ev;
        while (take_input_event(ev)) app.on_input(ev, canvas);

        // The history grows as laps are scored, so the view is handed over
        // every frame rather than once at startup.
        if (d.lap_history) {
            app.set_lap_history(d.lap_history->ms, d.lap_history->count,
                                d.lap_history->best_ms);
        }

        // One coherent read of every slot, once per frame.
        const core::state::Frame frame = core::state::snapshot(*d.slots, now);
        app.update(frame);

        // Each band is a full repaint of its own strip: the buffer has no
        // memory of what was on the panel a frame ago, so the dirty flags
        // save the widget's work, not the pixels.
        for (int16_t y = 0; y < ui::kScreenH; y += kBandLines) {
            canvas.set_origin(0, y);
            app.render(canvas, true);
            if (d.panel) {
                const hal::Status s = d.panel->blit(0, static_cast<uint16_t>(y),
                                                    ui::kScreenW, kBandLines,
                                                    band);
                if (hal::ok(s)) d.health->report_ok(core::state::Dev::Display, now);
                else d.health->report_error(core::state::Dev::Display,
                                            static_cast<int32_t>(s), now);
            }
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

        vTaskDelay(pdMS_TO_TICKS(33));      // 30 Hz
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
