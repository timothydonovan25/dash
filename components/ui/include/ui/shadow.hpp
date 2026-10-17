// What the panel holds, so a redrawn widget sends only what changed.
//
// The panel's bus is the frame budget (ui/wire_cost.hpp), and a widget's
// damage rectangle is a coarse answer to "what changed": a speed going from
// 47 to 48 redraws the whole readout, most of which is the same pixels as
// before. With a copy of the glass in memory, render can compare each strip
// it has just drawn against it and send only the rows and columns that
// differ. Comparing 38 kB is a fraction of a millisecond of CPU; sending it
// at 10 MHz is 7.7 ms of bus.
//
// The copy is only as good as the last transfer. Anything that leaves the
// glass unknown -- a failed blit, a panel re-init -- must invalidate() it,
// and until a whole frame has been recorded again every diff sends its
// rectangle whole, as render did before there was a shadow.
#pragma once
#include <cstddef>
#include <cstdint>
#include "ui/geometry.hpp"

namespace ui {

class Shadow {
public:
    // The most parts one rectangle becomes. Past this the last part grows.
    static constexpr int kMaxParts = 16;

    // `store` is kScreenW x kScreenH, owned by the caller (PSRAM on the
    // board). Null is allowed: every diff then sends its rectangle whole.
    explicit Shadow(Color* store) : px_(store) {}

    // The glass is unknown. Diffs send whole rectangles until validate().
    void invalidate() { valid_ = false; }
    // A whole frame has been recorded and every part of it was sent.
    void validate() { valid_ = px_ != nullptr; }
    bool valid() const { return valid_; }

    // `px` (r.w x r.h, packed) has been sent whole; remember it.
    void record(const Rect& r, const Color* px);

    // `px` (r.w x r.h, packed) is about to be sent. Remembers it, and writes
    // to `out` the parts of `r` that differ from what the panel holds: zero
    // when nothing does. Rows are merged into one part when sending the
    // unchanged pixels between them is cheaper than another transfer, which
    // costs `overhead_px` pixels' worth of bus (ui::wire::overhead_px).
    int diff(const Rect& r, const Color* px, Rect* out, int max_out,
             uint32_t overhead_px);

    // Moves `part`'s pixels (inside `r`, of the packed r.w x r.h buffer
    // `px`) to the front of `px`, packed, ready for a blit. In place, so the
    // band buffer is reused. Parts from diff() may be packed in the order it
    // returned them, each after the last has been sent: a part's pixels
    // never land on a later part's.
    static void pack(const Rect& r, Color* px, const Rect& part);

private:
    Color* px_;
    bool   valid_ = false;
};

}  // namespace ui
