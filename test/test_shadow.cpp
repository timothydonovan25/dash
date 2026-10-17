#include "check.hpp"
#include "ui/shadow.hpp"
#include "ui/wire_cost.hpp"
#include <vector>

using namespace ui;

namespace {

constexpr uint32_t kOverhead = 30;   // about the P4 board's, at 10 MHz

struct Glass {
    std::vector<Color> store;
    Shadow shadow;
    Glass() : store(static_cast<size_t>(kScreenW) * kScreenH, 0), shadow(store.data()) {}
};

// A packed r.w x r.h buffer of one colour.
std::vector<Color> fill(const Rect& r, Color c) {
    return std::vector<Color>(static_cast<size_t>(r.w) * r.h, c);
}

Color& at(std::vector<Color>& px, const Rect& r, int x, int y) {
    return px[static_cast<size_t>(y - r.y) * r.w + (x - r.x)];
}

}  // namespace

int main() {
    CASE("an unknown glass sends the whole rectangle");
    {
        Glass g;
        const Rect r{10, 20, 100, 40};
        auto px = fill(r, 0x1234);
        Rect out[Shadow::kMaxParts];
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 1);
        CHECK(out[0].x == r.x && out[0].y == r.y && out[0].w == r.w && out[0].h == r.h);
    }

    CASE("a redraw that changes nothing sends nothing");
    {
        Glass g;
        g.shadow.validate();
        const Rect r{10, 20, 100, 40};
        auto px = fill(r, 0x1234);
        Rect out[Shadow::kMaxParts];
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 1);
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 0);
    }

    CASE("one changed pixel is one pixel on the wire");
    {
        Glass g;
        g.shadow.validate();
        const Rect r{10, 20, 100, 40};
        auto px = fill(r, 0);
        at(px, r, 57, 33) = 0xFFFF;
        Rect out[Shadow::kMaxParts];
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 1);
        CHECK(out[0].x == 57 && out[0].y == 33 && out[0].w == 1 && out[0].h == 1);
        // And the glass now has it, so the same picture again is nothing.
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 0);
    }

    CASE("changes close together are one transfer, far apart are two");
    {
        Glass g;
        g.shadow.validate();
        const Rect r{0, 0, 200, 100};
        auto px = fill(r, 0);
        // Two 10-px marks two rows apart: sending the gap (20 px) is cheaper
        // than a second transfer (30 px of overhead).
        for (int x = 50; x < 60; ++x) { at(px, r, x, 10) = 1; at(px, r, x, 13) = 1; }
        Rect out[Shadow::kMaxParts];
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 1);
        CHECK(out[0].y == 10 && out[0].h == 4 && out[0].x == 50 && out[0].w == 10);

        // Sixty rows apart, the gap is 590 px: two transfers.
        for (int x = 50; x < 60; ++x) { at(px, r, x, 20) = 2; at(px, r, x, 80) = 2; }
        const int n = g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead);
        CHECK(n == 2);
        CHECK(out[0].y == 20 && out[0].h == 1);
        CHECK(out[1].y == 80 && out[1].h == 1);
    }

    CASE("the parts cover every changed pixel and never overlap in rows");
    {
        Glass g;
        g.shadow.validate();
        const Rect r{30, 40, 160, 40};
        auto before = fill(r, 0);
        Rect out[Shadow::kMaxParts];
        g.shadow.record(r, before.data());
        uint32_t seed = 7;
        for (int round = 0; round < 50; ++round) {
            auto px = before;
            const int changes = 1 + round % 9;
            for (int k = 0; k < changes; ++k) {
                seed = seed * 1103515245u + 12345u;
                const int x = r.x + static_cast<int>((seed >> 8) % r.w);
                const int y = r.y + static_cast<int>((seed >> 20) % r.h);
                at(px, r, x, y) = static_cast<Color>(seed >> 3);
            }
            const int n = g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead);
            for (int y = r.y; y < r.bottom(); ++y) {
                for (int x = r.x; x < r.right(); ++x) {
                    if (at(px, r, x, y) == at(before, r, x, y)) continue;
                    bool covered = false;
                    for (int i = 0; i < n; ++i) covered |= out[i].contains(x, y);
                    CHECK(covered);
                }
            }
            for (int i = 1; i < n; ++i) CHECK(out[i].y >= out[i - 1].bottom());
            before = px;
        }
    }

    CASE("past the part limit the last part grows instead");
    {
        Glass g;
        g.shadow.validate();
        const Rect r{0, 0, 480, 40};
        auto px = fill(r, 0);
        // A changed pixel on every other row at alternating far ends: no
        // merge is ever worth it, so only the cap stops the parts.
        for (int y = 0; y < 40; y += 2) at(px, r, (y % 4) ? 0 : 479, y) = 1;
        Rect out[4];
        const int n = g.shadow.diff(r, px.data(), out, 4, kOverhead);
        CHECK(n == 4);
        CHECK(out[3].bottom() == 39);
    }

    CASE("pack moves each part to the front of the buffer, in order");
    {
        const Rect r{100, 50, 8, 6};
        std::vector<Color> px(static_cast<size_t>(r.w) * r.h);
        for (int y = 0; y < r.h; ++y) {
            for (int x = 0; x < r.w; ++x) {
                px[static_cast<size_t>(y) * r.w + x] = static_cast<Color>((y << 8) | x);
            }
        }
        const auto orig = px;
        const Rect a{102, 50, 3, 2};    // rows 0-1, columns 2-4
        const Rect b{101, 53, 6, 3};    // rows 3-5, columns 1-6
        Shadow::pack(r, px.data(), a);
        for (int y = 0; y < a.h; ++y) {
            for (int x = 0; x < a.w; ++x) {
                CHECK(px[static_cast<size_t>(y) * a.w + x] == ((y << 8) | (x + 2)));
            }
        }
        // b's rows are untouched by packing a, so it packs right after.
        Shadow::pack(r, px.data(), b);
        for (int y = 0; y < b.h; ++y) {
            for (int x = 0; x < b.w; ++x) {
                CHECK(px[static_cast<size_t>(y) * b.w + x] ==
                      orig[static_cast<size_t>(y + 3) * r.w + x + 1]);
            }
        }
    }

    CASE("invalidate goes back to whole rectangles until validated");
    {
        Glass g;
        g.shadow.validate();
        const Rect r{0, 0, 50, 10};
        auto px = fill(r, 5);
        Rect out[Shadow::kMaxParts];
        g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead);
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 0);
        g.shadow.invalidate();
        CHECK(g.shadow.diff(r, px.data(), out, Shadow::kMaxParts, kOverhead) == 1);
        CHECK(out[0].w == 50 && out[0].h == 10);
    }

    CASE("no store: always whole, never valid");
    {
        Shadow s(nullptr);
        s.validate();
        CHECK(!s.valid());
        const Rect r{0, 0, 4, 4};
        auto px = fill(r, 1);
        Rect out[2];
        CHECK(s.diff(r, px.data(), out, 2, kOverhead) == 1);
        CHECK(s.diff(r, px.data(), out, 2, kOverhead) == 1);
    }

    CASE("the merge threshold comes from the wire model");
    {
        // 10 MHz, 8 bits, two bytes a pixel: 200 ns a pixel against 6.1 us
        // of fixed cost per transfer.
        const uint32_t k = wire::overhead_px(wire::i80_8bit(10000000u));
        CHECK(k >= 25 && k <= 35);
    }

    return check::report("shadow");
}
