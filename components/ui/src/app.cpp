#include "ui/app.hpp"
#include "ui/chrome.hpp"

namespace ui {

using core::nav::Button;
using core::nav::Mode;
using core::nav::Press;

namespace {
// The alert covers the middle of the screen, not all of it: the rider should
// still be able to see their speed while deciding what to do about a tyre.
constexpr Rect kAlertBox{20, 170, 280, 120};
}  // namespace

App::App(const Context& ctx) : ctx_(ctx) {
    Screen* all[] = {&speed_, &minimal_, &trip_, &trip_map_, &lap_, &lap_history_,
                     &track_map_, &all_data_, &engine_, &attitude_, &sensors_,
                     &gps_, &system_, &bus_, &radio_, &config_};
    for (Screen* s : all) s->bind(ctx_);
    alert_.set_bounds(kAlertBox);
}

Screen* App::screen_for(Mode m, uint8_t index) {
    switch (m) {
        case Mode::Cruise:
            switch (index) {
                case 0: return &speed_;
                case 1: return &trip_;
                case 2: return &trip_map_;
                default: return &minimal_;
            }
        case Mode::Track:
            switch (index) {
                case 0: return &lap_;
                case 1: return &lap_history_;
                default: return &track_map_;
            }
        case Mode::Stat:
            switch (index) {
                case 0: return &all_data_;
                case 1: return &engine_;
                default: return &attitude_;
            }
        case Mode::Diag:
            switch (index) {
                case 0: return &sensors_;
                case 1: return &gps_;
                case 2: return &system_;
                case 3: return &bus_;
                default: return &radio_;
            }
        case Mode::Config:
        case Mode::Count:
        default:
            return &config_;
    }
}

Screen& App::current() { return *screen_for(nav_.mode, nav_.screen); }

void App::enter_current(Canvas& c) {
    Screen& s = current();
    if (entered_ == &s) return;
    if (entered_) entered_->leave();
    s.enter(c);
    entered_ = &s;
    needs_full_ = true;
}

void App::begin(Canvas& c) {
    enter_current(c);
}

void App::on_input(const core::nav::InputEvent& e, Canvas& c) {
    // The alert owns E while it is up. A warning the rider has not seen must
    // not be dismissed by a press meant for something else, and a warning
    // they have seen must go away on the first press they make.
    if (alert_.active()) {
        if (e.button == Button::E) {
            alert_.dismiss();
            needs_full_ = true;
            return;
        }
    }

    Screen& s = current();
    if (s.handle(e)) return;

    const uint8_t focus = s.focus_count();
    const core::nav::State before = nav_;
    nav_ = core::nav::step(nav_, e, layout_, focus);

    if (nav_.focus != before.focus) s.set_focus(nav_.focus);
    if (nav_.mode != before.mode || nav_.screen != before.screen) {
        enter_current(c);
        current().set_focus(nav_.focus);
    }
}

void App::update(const core::state::Frame& f) {
    if (!entered_) return;
    current().update(f);
}

void App::render(Canvas& c, bool full) {
    if (!entered_) return;

    const bool repaint = full || needs_full_;
    if (repaint) {
        c.fill(color::kBg);
        // Drawn here rather than by each screen: the App is the only thing
        // that knows both halves, and fifteen hardcoded title strings would
        // be fifteen chances to disagree with the screen's own name().
        if (current().wants_title()) {
            chrome::title(c, mode_name(), current().name());
        }
        current().draw_background(c);
        // A band-based repaint has no memory of what was on the panel, so
        // everything has to be redrawn, not just what changed.
        for (Widget* w : current().widgets()) if (w) w->touch();
        needs_full_ = false;
    }

    for (Widget* w : current().widgets()) {
        if (!w || !w->dirty) continue;
        w->render(c);
        w->clear_dirty();
    }

    // Last, and over everything: the one widget allowed to break the
    // one-screen-at-a-time rule (§7.4).
    if (alert_.active()) {
        alert_.render(c);
        alert_.clear_dirty();
    }
}

}  // namespace ui
