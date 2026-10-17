// What a frame costs on the wire.
//
// The panel's bus is this dashboard's binding constraint, not the CPU and
// not the memory. Over the S3 board's SPI a whole 480x320 frame is 460 kB at
// 18-bit colour, 92 ms at 40 MHz — 11 fps no matter how fast anything
// draws. The P4 board's 8-bit 8080 bus (`i80_8bit`) is about three times
// faster, which still does not make a whole frame free. So the only
// question that matters about a screen is how many pixels it actually
// sends, and this prices that.
//
// It is a model, not a measurement. It knows the wire rate exactly, because
// that is arithmetic; the per-transfer overhead is an estimate and is meant
// to be replaced with a measured number once there is a board to measure.
// Everything it reports is per frame, so a screen can be judged against the
// 8.33 ms that render's 120 Hz allows.
#pragma once
#include <cstdint>
#include "ui/blit_plan.hpp"

namespace ui::wire {

// One panel on one bus. The defaults model the superseded S3 board's 40 MHz
// SPI panel, which test_wire pins; the P4 board is i80_8bit() below.
struct Panel {
    // 40 MHz, the S3 board's SPI clock.
    uint32_t clock_hz = 40000000u;

    // RGB666 (S3/SPI only). The ILI9488 in SPI mode does not accept RGB565, so every
    // 16-bit pixel in the band buffer becomes three bytes on the wire.
    uint8_t bytes_per_px = 3;

    // CASET (1 + 4) + PASET (1 + 4) + RAMWR (1). Paid once per transfer,
    // which is why splitting a region into strips is not free.
    uint8_t cmd_bytes = 11;

    // esp_lcd queueing, DMA descriptor setup and the D/C toggles around the
    // command bytes. A guess, and the one number here most worth replacing
    // with a scope reading — see docs/bringup.md.
    uint32_t overhead_ns = 5000;

    // The RGB565 -> RGB666 conversion in the atanisoft component is a CPU
    // loop over every pixel (esp_lcd_ili9488.c, panel_ili9488_draw_bitmap),
    // writing into one shared buffer. It therefore runs before the transfer
    // it feeds rather than alongside it, and adds to the frame instead of
    // hiding under it. About ten mask/shift ops per pixel at 240 MHz.
    uint32_t convert_ns_per_px = 50;

    // spi_bus_config_t::max_transfer_sz. A transfer larger than this is
    // rejected by the SPI driver at runtime, so the model counts them
    // rather than pricing a transfer that would never happen. Zero means
    // do not check.
    uint32_t bus_limit_bytes = 0;

    // How many bits move per clock. 1 for SPI, the default. An 8-bit 8080
    // bus moves a whole byte per write strobe.
    uint8_t bits_per_clock = 1;
};

// An 8-bit 8080 bus driving the ILI9488 in RGB565, as on the P4 board. No
// colour conversion — the pixels go out as they are — but the panel driver
// copies each transfer into its aligned DMA staging buffer first, which
// costs about as much per pixel as a memcpy does.
inline Panel i80_8bit(uint32_t write_hz) {
    Panel p;
    p.clock_hz = write_hz;
    p.bytes_per_px = 2;
    p.bits_per_clock = 8;
    p.convert_ns_per_px = 2;
    return p;
}

struct Cost {
    uint32_t transfers = 0;
    uint32_t pixels    = 0;
    uint64_t ns        = 0;
    // Transfers whose payload exceeds Panel::bus_limit_bytes. Not a
    // slow frame — a frame that does not happen.
    uint32_t oversized = 0;

    float ms() const { return static_cast<float>(ns) / 1e6f; }
    // The rate this frame could sustain if every frame cost the same.
    float fps() const { return ns > 0 ? 1e9f / static_cast<float>(ns) : 0.0f; }

    Cost& operator+=(const Cost& o) {
        transfers += o.transfers;
        pixels    += o.pixels;
        ns        += o.ns;
        oversized += o.oversized;
        return *this;
    }
};

// Prices one transfer of `pixels` pixels.
inline Cost transfer_cost(const Panel& p, uint32_t pixels) {
    Cost c;
    if (pixels == 0) return c;
    const uint64_t payload = static_cast<uint64_t>(pixels) * p.bytes_per_px;
    const uint64_t bits = (payload + p.cmd_bytes) * 8u;
    c.transfers = 1;
    c.pixels    = pixels;
    c.ns = p.overhead_ns
         + bits * 1000000000ull / (static_cast<uint64_t>(p.clock_hz) * p.bits_per_clock)
         + static_cast<uint64_t>(pixels) * p.convert_ns_per_px;
    if (p.bus_limit_bytes != 0 && payload > p.bus_limit_bytes) c.oversized = 1;
    return c;
}

// What one more transfer costs, in pixels' worth of bus: the point at which
// sending some unchanged pixels beats splitting a region in two
// (ui::Shadow::diff). About 30 on the P4 board at 10 MHz.
inline uint32_t overhead_px(const Panel& p) {
    const uint64_t per_byte_ns =
        1000000000ull / (static_cast<uint64_t>(p.clock_hz) * p.bits_per_clock / 8u);
    const uint64_t px_ns = per_byte_ns * p.bytes_per_px + p.convert_ns_per_px;
    const uint64_t fixed_ns = p.overhead_ns + per_byte_ns * p.cmd_bytes;
    return px_ns ? static_cast<uint32_t>(fixed_ns / px_ns) : 0;
}

// Accumulates a frame, then a run of frames.
//
// Feed it the same decisions the render task makes: `full_repaint()` when
// App::needs_full_repaint(), otherwise `region()` for every dirty widget.
class Model {
public:
    Model(const Panel& panel, size_t band_pixels, int16_t band_lines)
        : panel_(panel), band_pixels_(band_pixels), band_lines_(band_lines) {}

    // One dirty widget, split exactly the way blit_widget splits it.
    // Returns false if the region cannot be sent at all, which is a real
    // outcome and not a rounding error.
    bool region(const Rect& bounds) {
        Rect strips[kMaxStrips];
        const int n = plan_strips(bounds, band_pixels_, strips, kMaxStrips);
        if (n < 0) { ++unsendable_; return false; }
        for (int i = 0; i < n; ++i) {
            frame_ += transfer_cost(
                panel_, static_cast<uint32_t>(strips[i].w) * strips[i].h);
        }
        return true;
    }

    // One transfer of exactly `r`, already clipped and inside the band:
    // what render sends for each part ui::Shadow::diff() finds.
    void transfer(const Rect& r) {
        if (r.empty()) return;
        frame_ += transfer_cost(panel_, static_cast<uint32_t>(r.w) * r.h);
    }

    // The whole screen, band by band. Mirrors the full-repaint branch of
    // render_service: a fixed band height, not clipped at the last one.
    void full_repaint() {
        for (int16_t y = 0; y < kScreenH; y = static_cast<int16_t>(y + band_lines_)) {
            frame_ += transfer_cost(
                panel_, static_cast<uint32_t>(kScreenW) * band_lines_);
        }
    }

    // Closes the frame and rolls it into the totals. Returns what it cost.
    Cost end_frame() {
        const Cost f = frame_;
        total_ += f;
        if (f.ns > peak_.ns) peak_ = f;
        ++frames_;
        frame_ = Cost{};
        return f;
    }

    const Cost& frame() const { return frame_; }
    const Cost& total() const { return total_; }
    const Cost& peak()  const { return peak_; }
    uint32_t    frames() const { return frames_; }
    uint32_t    unsendable() const { return unsendable_; }

    // Mean frame cost over the run, in milliseconds.
    float mean_ms() const {
        return frames_ ? static_cast<float>(total_.ns) / frames_ / 1e6f : 0.0f;
    }

    const Panel& panel() const { return panel_; }

private:
    Panel    panel_;
    size_t   band_pixels_;
    int16_t  band_lines_;
    Cost     frame_{};
    Cost     total_{};
    Cost     peak_{};
    uint32_t frames_ = 0;
    uint32_t unsendable_ = 0;
};

}  // namespace ui::wire
