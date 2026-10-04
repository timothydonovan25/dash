// The wire-cost model, and the strip planner underneath it.
//
// The planner is the interesting half: the render task blits through it, so
// a bug here is not a wrong number in a report, it is wrong pixels on the
// panel. The property that matters is that the strips tile the clipped
// rectangle exactly — every pixel sent once, none missed.
#include "check.hpp"
#include "ui/blit_plan.hpp"
#include "ui/wire_cost.hpp"

#include <vector>

using namespace ui;

namespace {

// Band buffer as render_service sizes it: 480 wide, 40 lines.
constexpr size_t kBand = static_cast<size_t>(kScreenW) * 40;

// Paints every planned strip into a whole-screen coverage map and checks it
// against the rectangle that should have been covered.
bool tiles_exactly(Rect r, size_t band, Rect want) {
    Rect strips[kMaxStrips];
    const int n = plan_strips(r, band, strips, kMaxStrips);
    if (n <= 0) return want.empty();

    std::vector<int> hits(static_cast<size_t>(kScreenW) * kScreenH, 0);
    for (int i = 0; i < n; ++i) {
        const Rect& s = strips[i];
        if (static_cast<size_t>(s.w) * s.h > band) return false;  // will not fit
        for (int16_t y = s.y; y < s.bottom(); ++y) {
            for (int16_t x = s.x; x < s.right(); ++x) {
                if (x < 0 || y < 0 || x >= kScreenW || y >= kScreenH) return false;
                ++hits[static_cast<size_t>(y) * kScreenW + x];
            }
        }
    }
    for (int16_t y = 0; y < kScreenH; ++y) {
        for (int16_t x = 0; x < kScreenW; ++x) {
            const int got = hits[static_cast<size_t>(y) * kScreenW + x];
            const int expect = want.contains(x, y) ? 1 : 0;
            if (got != expect) return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    CASE("rows_for divides the band by the width");
    {
        CHECK(rows_for(kScreenW, kBand) == 40);
        CHECK(rows_for(240, kBand) == 80);
        CHECK(rows_for(0, kBand) == 0);
        // A row wider than the whole buffer does not fit even once.
        CHECK(rows_for(480, 100) == 0);
    }

    CASE("a whole screen splits into eight full bands");
    {
        Rect strips[kMaxStrips];
        const int n = plan_strips(Rect{0, 0, kScreenW, kScreenH}, kBand,
                                  strips, kMaxStrips);
        CHECK(n == 8);
        for (int i = 0; i < n; ++i) {
            CHECK(strips[i].w == kScreenW);
            CHECK(strips[i].h == 40);
            CHECK(strips[i].y == i * 40);
        }
    }

    CASE("strips tile the rectangle exactly, with no gap and no overlap");
    {
        // This is the property the render task depends on. A gap is a stripe
        // of stale pixels; an overlap is wasted wire time.
        CHECK(tiles_exactly(Rect{0, 0, kScreenW, kScreenH}, kBand,
                            Rect{0, 0, kScreenW, kScreenH}));
        CHECK(tiles_exactly(Rect{176, 92, 142, 150}, kBand,     // the g-ball
                            Rect{176, 92, 142, 150}));
        CHECK(tiles_exactly(Rect{0, 25, 25, 295}, kBand,        // the tach stem
                            Rect{0, 25, 25, 295}));
        // A height that is not a whole number of strips: the last one is short.
        CHECK(tiles_exactly(Rect{10, 10, kScreenW - 20, 101}, kBand,
                            Rect{10, 10, kScreenW - 20, 101}));
    }

    CASE("a rectangle hanging off the screen is clipped, not wrapped");
    {
        // The ILI9488 accepts a window past its own edge and wraps it, so
        // the clip has to happen here.
        CHECK(tiles_exactly(Rect{-20, -10, 100, 50}, kBand, Rect{0, 0, 80, 40}));
        CHECK(tiles_exactly(Rect{kScreenW - 30, kScreenH - 20, 100, 100}, kBand,
                            Rect{kScreenW - 30, kScreenH - 20, 30, 20}));
    }

    CASE("nothing on screen plans nothing, and says so without erroring");
    {
        Rect strips[kMaxStrips];
        CHECK(plan_strips(Rect{600, 10, 20, 20}, kBand, strips, kMaxStrips) == 0);
        CHECK(plan_strips(Rect{10, 10, 0, 20}, kBand, strips, kMaxStrips) == 0);
    }

    CASE("a row too wide for the band is refused, not silently truncated");
    {
        Rect strips[kMaxStrips];
        const int n = plan_strips(Rect{0, 0, kScreenW, 10}, 100, strips, kMaxStrips);
        CHECK(n == static_cast<int>(PlanResult::RowTooWide));
    }

    CASE("too many strips for the caller's array is an error, not an overrun");
    {
        Rect strips[2];
        const int n = plan_strips(Rect{0, 0, kScreenW, kScreenH}, kBand, strips, 2);
        CHECK(n == static_cast<int>(PlanResult::TooManyStrips));
    }

    CASE("one transfer costs its bytes on the wire plus its own overheads");
    {
        wire::Panel p;   // 40 MHz, 3 B/px, 11 cmd bytes, 5 us, 50 ns/px
        const wire::Cost c = wire::transfer_cost(p, 19200);   // one full band

        // 19200 px * 3 B = 57600 B payload, + 11 command bytes.
        // (57611 * 8) bits / 40e6 = 11.5222 ms on the wire.
        // + 5 us driver + 19200 * 50 ns = 960 us of RGB565->RGB666.
        CHECK(c.transfers == 1);
        CHECK(c.pixels == 19200);
        CHECK_NEAR(c.ms(), 12.487, 0.01);
    }

    CASE("splitting a region costs more than sending it whole");
    {
        wire::Panel p;
        const wire::Cost one = wire::transfer_cost(p, 200);
        wire::Cost two = wire::transfer_cost(p, 100);
        two += wire::transfer_cost(p, 100);
        CHECK(two.pixels == one.pixels);
        CHECK(two.ns > one.ns);
        // Exactly one extra overhead and one extra command header.
        CHECK(two.ns - one.ns == p.overhead_ns + p.cmd_bytes * 8ull * 1000000000ull / p.clock_hz);
    }

    CASE("a full repaint is the 11 fps ceiling the architecture doc claims");
    {
        wire::Panel p;
        wire::Model m(p, kBand, 40);
        m.full_repaint();
        const wire::Cost c = m.end_frame();
        CHECK(c.transfers == 8);
        CHECK(c.pixels == static_cast<uint32_t>(kScreenW) * kScreenH);
        // Wire time alone is 92 ms; the conversion loop is what takes it to 100.
        CHECK(c.ms() > 90.0f);
        CHECK(c.fps() < 12.0f);
    }

    CASE("a transfer over the bus limit is counted, not priced as if it worked");
    {
        wire::Panel p;
        p.bus_limit_bytes = 320u * 40u * 3u;      // what bsp/buses.cpp sets
        // One 480-wide band is 57600 B, which does not fit that.
        const wire::Cost c = wire::transfer_cost(p, 19200);
        CHECK(c.oversized == 1);
        // And a band sized to the limit does fit.
        CHECK(wire::transfer_cost(p, 12800).oversized == 0);
    }

    CASE("an 8-bit 8080 bus moves a byte per strobe, two bytes a pixel");
    {
        const wire::Panel p = wire::i80_8bit(10000000u);
        const wire::Cost c = wire::transfer_cost(p, 19200);   // one full band

        // 19200 px * 2 B = 38400 B, + 11 command bytes, one byte per 100 ns
        // strobe = 3.8411 ms. + 5 us driver + 19200 * 2 ns of staging copy.
        CHECK_NEAR(c.ms(), 3.8845, 0.01);
    }

    CASE("a full repaint over 8080 at 10 MHz is just inside 30 Hz");
    {
        wire::Model m(wire::i80_8bit(10000000u), kBand, 40);
        m.full_repaint();
        const wire::Cost c = m.end_frame();
        CHECK(c.pixels == static_cast<uint32_t>(kScreenW) * kScreenH);
        // 307 kB at 10 MB/s is 30.7 ms on the wire alone.
        CHECK(c.ms() > 30.7f);
        CHECK(c.ms() < 33.3f);
    }

    CASE("the model accumulates frames and remembers the worst one");
    {
        wire::Panel p;
        wire::Model m(p, kBand, 40);
        m.region(Rect{0, 0, 100, 100});
        m.end_frame();
        m.region(Rect{0, 0, 200, 200});
        const wire::Cost big = m.end_frame();
        m.region(Rect{0, 0, 10, 10});
        m.end_frame();

        CHECK(m.frames() == 3);
        CHECK(m.peak().ns == big.ns);
        CHECK(m.total().pixels == 100u * 100 + 200u * 200 + 10u * 10);
        CHECK(m.mean_ms() > 0.0f);
    }

    CASE("a region that cannot be sent is reported rather than costed as free");
    {
        wire::Panel p;
        wire::Model m(p, 100, 40);            // a band too small for any row
        CHECK(!m.region(Rect{0, 0, kScreenW, 10}));
        CHECK(m.unsendable() == 1);
        CHECK(m.frame().transfers == 0);
    }

    return check::report("wire");
}
