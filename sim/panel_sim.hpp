// The panel as the bus delivers it.
//
// The window used to show the canvas the firmware had just drawn, whole and
// instantly, which is the one thing the real panel cannot do: the ILI9488
// has no framebuffer on our side of the bus, it shows whatever has arrived,
// and a pixel arrives when its two bytes have been clocked in. So a slow
// screen does not just cost frames, it is *seen* — a full repaint wipes down
// the panel over ~31 ms at 10 MHz, and a widget that is late shows its old
// value until its strip lands.
//
// This keeps two images. The canvas is what the firmware rendered. `shown_`
// is what the panel holds. Each frame's transfers are queued with the pixels
// they carry snapshotted at queue time (the real render task fills its band
// buffer before sending, so a later canvas change cannot reach a strip that
// is already on its way), and `advance()` plays them out in raster order at
// the cost `ui::wire` prices them: the same strip split, the same command
// overhead, the same clock. Nothing here decides what is sent; the caller
// queues what render_service would.
//
// While the bus is busy the render task is blocked in its blit, so the sim
// skips frames (`busy()`), which is how a slow bus turns into a low frame
// rate rather than an unbounded queue.
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

#include "ui/blit_plan.hpp"
#include "ui/canvas.hpp"
#include "ui/geometry.hpp"
#include "ui/wire_cost.hpp"

namespace sim {

class PanelSim {
public:
    PanelSim(size_t band_pixels, int16_t band_lines)
        : band_pixels_(band_pixels), band_lines_(band_lines),
          shown_(static_cast<size_t>(ui::kScreenW) * ui::kScreenH, 0) {}

    // Bus write clock. Zero turns the model off: transfers land at once.
    void set_clock_hz(uint32_t hz) {
        hz_ = hz;
        if (hz_ == 0) flush();
    }
    uint32_t clock_hz() const { return hz_; }
    void set_bus_limit(uint32_t bytes) { limit_ = bytes; }

    // True while transfers are still in flight. A frame must not start.
    bool busy() const { return hz_ != 0 && !q_.empty(); }

    // A widget's rectangle, split the way blit_widget splits it.
    void queue_region(const ui::Rect& r, const ui::Color* canvas) {
        ui::Rect strips[ui::kMaxStrips];
        const int n = ui::plan_strips(r, band_pixels_, strips, ui::kMaxStrips);
        for (int i = 0; i < n; ++i) queue(strips[i], canvas);
    }

    // The whole screen in fixed bands, as the full-repaint branch does.
    void queue_full(const ui::Color* canvas) {
        for (int16_t y = 0; y < ui::kScreenH; y = static_cast<int16_t>(y + band_lines_)) {
            const int16_t h = static_cast<int16_t>(
                y + band_lines_ > ui::kScreenH ? ui::kScreenH - y : band_lines_);
            queue(ui::Rect{0, y, ui::kScreenW, h}, canvas);
        }
    }

    // Plays `ns` of bus time. Pixels land in order, mid-strip included.
    void advance(uint64_t ns) {
        if (hz_ == 0) { flush(); return; }
        while (ns > 0 && !q_.empty()) {
            Transfer& t = q_.front();
            const uint64_t left = t.total_ns - t.done_ns;
            const uint64_t step = std::min(ns, left);
            t.done_ns += step;
            ns -= step;
            land(t);
            if (t.done_ns >= t.total_ns) q_.pop_front();
        }
    }

    // Everything outstanding arrives now.
    void flush() {
        for (Transfer& t : q_) { t.done_ns = t.total_ns; land(t); }
        q_.clear();
    }

    // RGB565, kScreenW x kScreenH: what the glass shows right now.
    const ui::Color* shown() const { return shown_.data(); }

    // Pixels still on the wire, for the status line.
    uint32_t pending_px() const {
        uint32_t n = 0;
        for (const Transfer& t : q_) n += static_cast<uint32_t>(t.rect.w) * t.rect.h;
        return n;
    }

private:
    struct Transfer {
        ui::Rect rect;
        std::vector<ui::Color> px;   // snapshot, rect.w * rect.h
        uint64_t total_ns = 0;
        uint64_t done_ns = 0;
        uint64_t overhead_ns = 0;
        uint32_t landed = 0;         // pixels already copied to the panel
    };

    void queue(const ui::Rect& r, const ui::Color* canvas) {
        Transfer t;
        t.rect = r;
        t.px.resize(static_cast<size_t>(r.w) * r.h);
        for (int16_t y = 0; y < r.h; ++y) {
            const ui::Color* src = canvas + static_cast<size_t>(r.y + y) * ui::kScreenW + r.x;
            std::copy(src, src + r.w, t.px.begin() + static_cast<size_t>(y) * r.w);
        }
        if (hz_ == 0) {
            t.total_ns = t.done_ns = 1;
            land(t);
            return;
        }
        ui::wire::Panel p = ui::wire::i80_8bit(hz_);
        p.bus_limit_bytes = limit_;
        t.total_ns = std::max<uint64_t>(
            1, ui::wire::transfer_cost(p, static_cast<uint32_t>(t.px.size())).ns);
        // The fixed part (command bytes, queueing) shows nothing on the
        // glass; the pixels stream in over what is left.
        t.overhead_ns = std::min<uint64_t>(p.overhead_ns, t.total_ns - 1);
        q_.push_back(std::move(t));
    }

    // Copies the pixels this transfer has delivered by now.
    void land(Transfer& t) {
        const uint64_t span = t.total_ns - t.overhead_ns;
        const uint64_t into = t.done_ns > t.overhead_ns ? t.done_ns - t.overhead_ns : 0;
        const uint64_t total_px = t.px.size();
        const uint32_t upto = static_cast<uint32_t>(
            t.done_ns >= t.total_ns ? total_px : total_px * into / span);
        for (uint32_t i = t.landed; i < upto; ++i) {
            const int x = t.rect.x + static_cast<int>(i % t.rect.w);
            const int y = t.rect.y + static_cast<int>(i / t.rect.w);
            shown_[static_cast<size_t>(y) * ui::kScreenW + x] = t.px[i];
        }
        t.landed = std::max(t.landed, upto);
    }

    size_t band_pixels_;
    int16_t band_lines_;
    uint32_t hz_ = 10000000u;
    uint32_t limit_ = 0;
    std::deque<Transfer> q_;
    std::vector<ui::Color> shown_;
};

}  // namespace sim
