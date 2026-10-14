// The widget base (§7.4).
//
// A widget owns a rectangle and a value. `set()` compares against what it is
// already showing and only marks itself dirty on a change — that is the old
// dirty-flag optimisation, in one place instead of scattered through every
// screen, and it is what keeps a 60 Hz repaint of a 320x480 panel affordable
// over the 8080 bus.
#pragma once
#include <cstddef>
#include "ui/canvas.hpp"

namespace ui {

class Widget {
public:
    virtual ~Widget() = default;

    Rect bounds;
    bool dirty = true;

    virtual void render(Canvas& c) = 0;

    // What a partial repaint has to send, in whole-screen coordinates: the
    // whole bounds unless the widget knows better. A table that changed one
    // row, or a tach bar whose marker moved a few pixels, says so here, and
    // only those rectangles cross the 8-bit bus; render() is then run with
    // the canvas window on each, so it draws the same pixels it always
    // would and the window throws the rest away. At most `max` rectangles;
    // a widget with more returns its bounds instead.
    virtual int damage(Rect* out, int max) const {
        (void)max;
        out[0] = bounds;
        return 1;
    }

    void set_bounds(const Rect& r) { bounds = r; touch(); }
    // The whole widget needs sending.
    void touch() { dirty = true; whole_ = true; }
    virtual void clear_dirty() { dirty = false; whole_ = false; }

protected:
    // Part of it does, and the widget's damage() says which part. Anything
    // that also calls touch() before the next clear wins: whole beats part.
    void touch_part() { dirty = true; }
    bool whole_ = true;

    // Every set() in every widget funnels through one of these, so "did it
    // change" is answered the same way everywhere.
    bool assign(float& field, float v) {
        if (field == v) return false;
        field = v;
        touch();
        return true;
    }
    template <typename T>
    bool assign_t(T& field, T v) {
        if (field == v) return false;
        field = v;
        touch();
        return true;
    }
};

// A screen hands back its widgets as a plain pointer and a count. std::span
// would be the C++20 spelling; this codebase is C++17 and this is the whole
// of what it needs.
struct WidgetList {
    Widget* const* items = nullptr;
    size_t         count = 0;

    Widget* const* begin() const { return items; }
    Widget* const* end() const { return items + count; }
};

}  // namespace ui
