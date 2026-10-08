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
// Centred, and clear of the shift-light strip along the top.
constexpr Rect kBootBox{90, 100, 300, 110};
// Top-right corner: small, but on every screen.
constexpr Rect kDevTag{448, 0, 32, 11};

core::ota::Key key_of(Button b) {
    return static_cast<core::ota::Key>(static_cast<uint8_t>(b));
}
}  // namespace

App::App(const Context& ctx) : ctx_(ctx) {
    Screen* all[] = {&speed_, &media_, &minimal_, &trip_, &trip_map_, &lap_,
                     &lap_history_, &track_map_, &all_data_, &engine_,
                     &attitude_, &sensors_, &gps_, &system_, &bus_, &radio_,
                     &update_, &config_};
    for (Screen* s : all) s->bind(ctx_);
    alert_.set_bounds(kAlertBox);
    chooser_.set_bounds(kBootBox);
    dev_tag_.set_bounds(kDevTag);
}

Screen* App::screen_for(Mode m, uint8_t index) {
    switch (m) {
        case Mode::Cruise:
            switch (index) {
                case 0: return &speed_;
                case 1: return &media_;
                case 2: return &trip_;
                case 3: return &trip_map_;
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
                case 4: return &radio_;
                default: return &update_;
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
    // The boot window owns every button while it is up: the first press
    // opens the chooser, and none of them should also change the screen.
    if (boot_.active()) {
        boot_.press(key_of(e.button), last_t_ms_);
        return;
    }

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
    last_t_ms_ = f.t_ms;

    if (!boot_seen_ && f.ota.known) {
        boot_seen_ = true;
        if (f.ota.boot_window) {
            boot_.begin(f.t_ms, f.ota.other == core::ota::Other::Ready);
        }
    }
    const bool was_active = boot_.active();
    boot_.tick(f.t_ms);
    if (boot_.active()) {
        chooser_.set(boot_.phase() == core::ota::Phase::Chooser, boot_.focus(),
                     boot_.dev_available(),
                     static_cast<uint8_t>((boot_.left_ms(f.t_ms) + 999) / 1000),
                     f.ota.running_ver, f.ota.other_ver);
    } else if (was_active) {
        // Gone: whatever it covered has to come back.
        needs_full_ = true;
    }
    if (boot_.take_choice() == core::ota::Choice::Dev && ctx_.commands) {
        ctx_.commands->post(core::state::Command(core::state::CommandId::OtaBootDevOnce));
    }

    const bool dev = f.ota.known && f.ota.role != core::ota::Role::Stable;
    if (dev != dev_running_) {
        dev_running_ = dev;
        needs_full_ = true;
    }

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
    if (boot_.active()) {
        chooser_.render(c);
        chooser_.clear_dirty();
    }
    if (dev_running_) {
        dev_tag_.render(c);
        dev_tag_.clear_dirty();
    }
}

}  // namespace ui
