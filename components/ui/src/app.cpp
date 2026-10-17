#include "ui/app.hpp"
#include "core/sweep/sweep.hpp"
#include "ui/chrome.hpp"
#include "ui/theme.hpp"
#include <cstdio>
#include <cstring>

namespace ui {

using core::nav::Button;
using core::nav::Mode;
using core::nav::Press;

namespace {
// The alert covers the middle of the screen, not all of it: the rider should
// still be able to see their speed while deciding what to do about a tyre.
constexpr Rect kAlertBox{20, 170, 280, 120};
// Across the whole top, where the eye already goes for the tach.
constexpr Rect kSlipBar{0, 0, kScreenW, 30};
// Top-right corner: small, but on every screen.
constexpr Rect kDevTag{448, 0, 32, 11};
// The splash's progress bar: a track that fills over kSplashMs.
constexpr int16_t kSplashBarW = 300, kSplashBarH = 8, kSplashBarY = 220;
constexpr Rect kSplashBar{static_cast<int16_t>(kScreenW / 2 - kSplashBarW / 2), kSplashBarY,
                          kSplashBarW, kSplashBarH};
// The most rectangles one widget may ask for before it is sent whole.
constexpr int kMaxPerWidget = 6;
}  // namespace

App::App(const Context& ctx) : ctx_(ctx) {
    Screen* all[] = {&speed_, &media_, &minimal_, &trip_, &trip_map_, &nav_screen_, &lap_,
                     &lap_history_, &track_map_, &all_data_, &engine_,
                     &attitude_, &sensors_, &gps_, &system_, &power_, &bus_, &radio_,
                     &update_, &config_, &gearcal_};
    for (Screen* s : all) s->bind(ctx_);
    alert_.set_bounds(kAlertBox);
    slip_bar_.set_bounds(kSlipBar);
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
                case 4: return &nav_screen_;
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
                case 3: return &power_;
                case 4: return &bus_;
                case 5: return &radio_;
                default: return &update_;
            }
        case Mode::Config:
        case Mode::Count:
        default:
            return cal_active_ ? static_cast<Screen*>(&gearcal_) : &config_;
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
            alerter_.acknowledge();
            alert_kind_ = core::alert::Kind::None;
            alert_.dismiss();
            restore(kAlertBox);
            return;
        }
    }

    Screen& s = current();
    if (s.handle(e)) {
        const int f = s.focus_now();
        if (f >= 0) nav_.focus = static_cast<uint8_t>(f);
        return;
    }

    const uint8_t focus = s.focus_count();
    const core::nav::State before = nav_;
    nav_ = core::nav::step(nav_, e, layout_, focus);

    if (nav_.focus != before.focus) s.set_focus(nav_.focus);
    if (nav_.mode != before.mode || nav_.screen != before.screen) {
        enter_current(c);
        current().set_focus(nav_.focus);
    }
}

void App::update_alert(const core::state::Frame& f) {
    using core::alert::Kind;
    const Kind k = alerter_.update(f);
    // Nothing is drawn over the name card or the gauge sweep, and the
    // alerter has kept its clock meanwhile, so a fault present at key-on is
    // shown the moment the dash is.
    if (splash_on_ || !sweep_done_) return;
    if (k == alert_kind_) return;
    alert_kind_ = k;
    if (k == Kind::None) {
        // Whatever it covered has to be painted back: that box, not the
        // whole screen.
        alert_.dismiss();
        restore(kAlertBox);
    } else {
        const bool grave = k == Kind::BikeDown || k == Kind::BatteryHigh;
        alert_.show(core::alert::text(k), grave ? color::kAlarm : color::kWarn);
    }
}

void App::update_slip(const core::state::Frame& f) {
    core::slip::Reading r;
    const bool on = !ctx_.settings || ctx_.settings->slip_bar;
    // Not over the name card or the sweep, which own the whole screen.
    if (on && !splash_on_ && sweep_done_) {
        const bool fused = f.fused.speed_valid;
        const float road = fused ? f.fused.speed_mph : f.gnss.speed_mph;
        // A wheel sensor the firmware has faulted reads 0; leave it out
        // rather than call it a lock (core::slip::Meter::update).
        const bool front_ok = f.health[core::state::Dev::FrontWheel].state !=
                              core::state::DevState::Fault;
        const bool rear_ok = f.health[core::state::Dev::RearWheel].state !=
                             core::state::DevState::Fault;
        r = slip_meter_.update(f.motion.front_mph, front_ok, f.motion.rear_mph,
                               rear_ok, road, fused || f.gnss.fix, f.t_ms,
                               core::slip::Config{});
    }
    const bool was = slip_bar_.active();
    slip_bar_.set(r.kind, r.pct);
    // The bar covered the top of the screen, which is title and chrome, not
    // only widgets: put that strip back, from the ground up. On a dirt bike
    // this happens every time a spin or a lock ends, so a whole frame (two
    // frame periods of wire) each time was a stutter the rider could see.
    if (was && !slip_bar_.active()) restore(kSlipBar);
}

void App::update(const core::state::Frame& f) {
    if (!entered_) return;
    update_alert(f);
    update_slip(f);
    const bool dev = f.ota.known && f.ota.role != core::ota::Role::Stable;
    if (dev != dev_running_) {
        dev_running_ = dev;
        needs_full_ = true;
    }

    // CONFIG hands itself over to the gear calibration page while `logic`
    // has a run open, and takes itself back at the end. Entering needs a
    // canvas, so the swap itself waits for render(); until then the old
    // screen is still the one being shown.
    const bool cal = f.gear_cal.phase != 0 && nav_.mode == Mode::Config;
    if (cal != cal_active_) {
        cal_swap_ = true;
        if (!cal) config_.resume();
        cal_active_ = cal;
        return;
    }

    // The splash first, timed from the first frame like the sweep. It
    // draws itself and nothing else, so the screen underneath only starts
    // being fed frames once it is over.
    if (!splash_started_) {
        splash_started_ = true;
        splash_start_ms_ = f.t_ms;
        splash_on_ = ctx_.settings && ctx_.settings->splash;
    }
    if (splash_on_) {
        const uint32_t t = f.t_ms - splash_start_ms_;
        if (t < kSplashMs) {
            // The card is painted whole once; after that only the bar
            // moves, so only the bar goes to the panel (a whole frame each
            // time was 31 ms of wire, every frame, for a second and a half).
            const uint16_t pm = static_cast<uint16_t>(t * 1000u / kSplashMs);
            if (pm != splash_permille_) {
                splash_permille_ = pm;
                splash_bar_dirty_ = true;
            }
            char ver[sizeof splash_ver_];
            std::snprintf(ver, sizeof ver, "%s", f.ota.known ? f.ota.running_ver : "");
            if (std::strcmp(ver, splash_ver_) != 0) {
                std::memcpy(splash_ver_, ver, sizeof ver);
                needs_full_ = true;
            }
            return;
        }
        splash_on_ = false;
        needs_full_ = true;
    }

    // The start-up sweep (core/sweep) rewrites the frame the screen sees,
    // timed from the first frame drawn rather than from boot, so the whole
    // sweep is on the panel however long the hardware took to come up. Once
    // it has ended it stays ended.
    if (!sweep_done_) {
        if (!sweep_started_) {
            sweep_started_ = true;
            sweep_start_ms_ = f.t_ms;
        }
        core::sweep::Config sc;
        if (const core::config::Settings* s = ctx_.settings) {
            sc.enabled = s->gauge_sweep;
            sc.duration_ms = s->sweep_ms;
            sc.peak_rpm = s->shift_flash_rpm;
            sc.gears = s->gear_count;
        }
        core::state::Frame swept = f;
        if (core::sweep::apply(swept, f.t_ms - sweep_start_ms_, sc)) {
            current().update(swept);
            return;
        }
        sweep_done_ = true;
    }

    current().update(f);
}

void App::render_splash(Canvas& c) {
    c.fill(color::kBg);
    const int16_t cx = static_cast<int16_t>(kScreenW / 2);
    c.draw_text(cx, 96, "CRF250F", color::kAlarm, 8, Align::Center);
    c.draw_text(cx, 168, "DASH", theme::secondary, 3, Align::Center);
    if (splash_ver_[0]) {
        c.draw_text(cx, 270, splash_ver_, theme::secondary, 1, Align::Center);
    }
    // A bar that fills over the splash, so it reads as "starting", not hung.
    const Rect& track = kSplashBar;
    c.draw_rect(track, theme::rule);
    const int16_t w = static_cast<int16_t>((kSplashBarW - 4) * splash_permille_ / 1000);
    if (w > 0) {
        c.fill_rect(Rect{static_cast<int16_t>(track.x + 2), static_cast<int16_t>(track.y + 2), w,
                         static_cast<int16_t>(kSplashBarH - 4)},
                    color::kAlarm);
    }
}

void App::restore(const Rect& r) {
    for (int i = 0; i < restore_n_; ++i) {
        if (restore_[i].x == r.x && restore_[i].y == r.y &&
            restore_[i].w == r.w && restore_[i].h == r.h) return;
    }
    if (restore_n_ < kMaxRestore) restore_[restore_n_++] = r;
    else needs_full_ = true;
}

void App::paint_region(Canvas& c) {
    // What render(c, true) draws, for whatever part of the screen the
    // canvas is windowed onto, without touching any widget's dirty state:
    // this is one rectangle among the frame's damage, not a new frame.
    c.fill(color::kBg);
    if (current().wants_title()) chrome::title(c, mode_name(), current().name());
    current().draw_background(c);
    for (Widget* w : current().widgets()) {
        if (w && c.intersects(w->bounds)) w->render(c);
    }
    if (slip_bar_.active() && c.intersects(slip_bar_.bounds)) slip_bar_.render(c);
    if (alert_.active() && c.intersects(alert_.bounds)) alert_.render(c);
    if (dev_running_ && c.intersects(dev_tag_.bounds)) dev_tag_.render(c);
}

void App::render(Canvas& c, bool full) {
    if (!entered_) return;
    if (cal_swap_) {
        cal_swap_ = false;
        enter_current(c);
    }
    if (splash_on_) {
        render_splash(c);
        return;
    }

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
        restore_n_ = 0;
    }

    // A widget outside the canvas's band is still cleared: on a band-by-band
    // repaint the band that does hold it has drawn it, or will.
    for (Widget* w : current().widgets()) {
        if (!w || !w->dirty) continue;
        if (c.intersects(w->bounds)) w->render(c);
        w->clear_dirty();
    }

    if (slip_bar_.active()) {
        if (c.intersects(slip_bar_.bounds)) slip_bar_.render(c);
        slip_bar_.clear_dirty();
    }

    // Last, and over everything: the one widget allowed to break the
    // one-screen-at-a-time rule (§7.4).
    if (alert_.active()) {
        if (c.intersects(alert_.bounds)) alert_.render(c);
        alert_.clear_dirty();
    }
    if (dev_running_) {
        if (c.intersects(dev_tag_.bounds)) dev_tag_.render(c);
        dev_tag_.clear_dirty();
    }
}

size_t App::damage(Damage* out, size_t max) {
    if (!entered_ || needs_full_ || max == 0) return 0;
    size_t n = 0;
    if (splash_on_) {
        if (splash_bar_dirty_) out[n++] = Damage{nullptr, kSplashBar};
        return n;
    }

    // Uncovered patches first; a widget inside one is drawn again by its own
    // damage below, the same pixels twice rather than a missing one.
    for (int i = 0; i < restore_n_ && n < max; ++i) {
        Damage dm;
        dm.r = restore_[i];
        dm.region = true;
        out[n++] = dm;
    }

    for (Widget* w : current().widgets()) {
        if (!w || !w->dirty) continue;
        Rect rs[kMaxPerWidget];
        const int k = w->damage(rs, kMaxPerWidget);
        if (n + static_cast<size_t>(k) > max) {
            // More than a frame's worth of pieces: send the lot.
            needs_full_ = true;
            return 0;
        }
        for (int i = 0; i < k; ++i) out[n++] = Damage{w, rs[i]};
    }
    // The overlays go only when they changed themselves. Where a widget
    // under them changed, draw_damage() has already put them back on top.
    if (slip_bar_.active() && slip_bar_.dirty && n < max) out[n++] = Damage{&slip_bar_, slip_bar_.bounds};
    if (alert_.active() && alert_.dirty && n < max) out[n++] = Damage{&alert_, alert_.bounds};
    if (dev_running_ && dev_tag_.dirty && n < max) out[n++] = Damage{&dev_tag_, dev_tag_.bounds};
    return n;
}

void App::draw_damage(Canvas& c, const Damage& d) {
    if (d.region) {
        paint_region(c);
        return;
    }
    if (!d.w) {
        render_splash(c);
        return;
    }
    d.w->render(c);
    // Over everything, as on a full repaint: an overlay whose own pixels
    // did not change still has to cover what was just drawn under it.
    if (d.w != &slip_bar_ && d.w != &alert_ && d.w != &dev_tag_ && slip_bar_.active() &&
        d.r.intersects(slip_bar_.bounds)) {
        slip_bar_.render(c);
    }
    if (d.w != &alert_ && alert_.active() && d.r.intersects(alert_.bounds)) alert_.render(c);
    if (d.w != &dev_tag_ && dev_running_ && d.r.intersects(dev_tag_.bounds)) dev_tag_.render(c);
}

void App::clear_damage() {
    splash_bar_dirty_ = false;
    restore_n_ = 0;
    for (Widget* w : current().widgets()) if (w) w->clear_dirty();
    slip_bar_.clear_dirty();
    alert_.clear_dirty();
    dev_tag_.clear_dirty();
}

}  // namespace ui
