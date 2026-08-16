// The widget base (§7.4).
//
// A widget owns a rectangle and a value. `set()` compares against what it is
// already showing and only marks itself dirty on a change — that is the old
// dirty-flag optimisation, in one place instead of scattered through every
// screen, and it is what keeps a 30 Hz repaint of a 320x480 panel affordable
// over SPI.
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

    void set_bounds(const Rect& r) { bounds = r; touch(); }
    void touch() { dirty = true; }
    void clear_dirty() { dirty = false; }

protected:
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
