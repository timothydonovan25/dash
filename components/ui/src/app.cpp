#include "ui/app.hpp"
#include "core/stack.hpp"
#include "core/sweep/sweep.hpp"
#include "ui/chrome.hpp"
#include "ui/theme.hpp"
#include "core/media/feed.hpp"
#include "core/stunt/stunt.hpp"
#include "core/units/units.hpp"
#include <cstdio>
#include <cstring>
#include <new>

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
// Along the bottom, the full width: under the eye's path to the speed, and
// clear of the tach and the slip bar at the top.
constexpr Rect kToastBox{0, 266, kScreenW, 54};
// How long a finished event stays up.
constexpr uint32_t kPerfToastMs = 4000;
constexpr uint32_t kRecoveryToastMs = 6000;   // parked: time to read it
constexpr uint32_t kStuntToastMs = 3500;
constexpr uint32_t kNotifyToastMs = 8000;
// A text message waits longer, for E: a minute, or until it is cleared.
constexpr uint32_t kMessageToastMs = 60000;
constexpr uint32_t kLineToastMs = 3000;
// Riding, for whether a message's text shows (Settings::notify_text_moving).
constexpr float kMovingMph = 3.0f;
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
    alert_.set_bounds(kAlertBox);
    slip_bar_.set_bounds(kSlipBar);
    toast_.set_bounds(kToastBox);
    dev_tag_.set_bounds(kDevTag);
}

App::~App() { destroy_active(); }

ScreenId App::screen_for(Mode m, uint8_t index) const {
    switch (m) {
        case Mode::Cruise:
            switch (index) {
                case 0: return ScreenId::Speed;
                case 1: return ScreenId::Media;
                case 2: return ScreenId::Trip;
                case 3: return ScreenId::TripMap;
                case 4: return ScreenId::Nav;
                case 5: return ScreenId::Minimal;
                default: return ScreenId::Phone;
            }
        case Mode::Track:
            switch (index) {
                case 0: return ScreenId::Race;
                case 1: return ScreenId::Lap;
                case 2: return ScreenId::LapHistory;
                case 3: return ScreenId::TrackMap;
                default: return ScreenId::Session;
            }
        case Mode::Stat:
            switch (index) {
                case 0: return ScreenId::AllData;
                case 1: return ScreenId::Engine;
                case 2: return ScreenId::Attitude;
                case 3: return ScreenId::Perf;
                case 4: return ScreenId::Stunt;
                default: return ScreenId::Heart;
            }
        case Mode::Diag:
            switch (index) {
                case 0: return ScreenId::DiagMenu;
                case 1: return ScreenId::Sensors;
                case 2: return ScreenId::Gps;
                case 3: return ScreenId::Fusion;
                case 4: return ScreenId::System;
                case 5: return ScreenId::Power;
                case 6: return ScreenId::Bus;
                case 7: return ScreenId::Radio;
                default: return ScreenId::Update;
            }
        case Mode::Config:
        case Mode::Count:
        default:
            return cal_active_ ? ScreenId::GearCal : ScreenId::Config;
    }
}

void App::destroy_active() {
    if (!active_) return;
    if (active_id_ == ScreenId::Config) {
        config_memo_ = static_cast<ConfigScreen*>(active_)->memo();
    }
    active_->leave();
    active_->~Screen();
    active_ = nullptr;
    active_id_ = ScreenId::None;
}

namespace {
template <typename T>
T* make(unsigned char* slot) {
    static_assert(sizeof(T) <= AllScreens::size && alignof(T) <= AllScreens::align,
                  "a screen missing from ui::AllScreens");
    return new (slot) T();
}
}  // namespace

void App::build(ScreenId id) {
    destroy_active();
    Screen* s = nullptr;
    switch (id) {
        case ScreenId::Speed:      s = make<SpeedScreen>(slot_); break;
        case ScreenId::Media:      s = make<MediaScreen>(slot_); break;
        case ScreenId::Trip:       s = make<TripScreen>(slot_); break;
        case ScreenId::TripMap: {
            auto* t = make<TripMapScreen>(slot_);
            t->set_source(trip_src_);
            s = t;
            break;
        }
        case ScreenId::Nav:        s = make<NavScreen>(slot_); break;
        case ScreenId::Minimal:    s = make<MinimalScreen>(slot_); break;
        case ScreenId::Phone:      s = make<PhoneScreen>(slot_); break;
        case ScreenId::Race:       s = make<RaceScreen>(slot_); break;
        case ScreenId::Lap:        s = make<LapScreen>(slot_); break;
        case ScreenId::Session:    s = make<SessionScreen>(slot_); break;
        case ScreenId::LapHistory: {
            auto* t = make<LapHistoryScreen>(slot_);
            t->set_history(laps_, lap_count_, best_lap_, lap_peaks_);
            s = t;
            break;
        }
        case ScreenId::TrackMap: {
            auto* t = make<TrackMapScreen>(slot_);
            t->set_source(track_src_);
            s = t;
            break;
        }
        case ScreenId::AllData:    s = make<AllDataScreen>(slot_); break;
        case ScreenId::Engine:     s = make<EngineScreen>(slot_); break;
        case ScreenId::Attitude:   s = make<AttitudeScreen>(slot_); break;
        case ScreenId::Perf:       s = make<PerfScreen>(slot_); break;
        case ScreenId::Stunt:      s = make<StuntScreen>(slot_); break;
        case ScreenId::Heart: {
            auto* t = make<HeartScreen>(slot_);
            t->set_history(hr_history_);
            s = t;
            break;
        }
        case ScreenId::DiagMenu:   s = make<DiagMenuScreen>(slot_); break;
        case ScreenId::Sensors:    s = make<SensorsScreen>(slot_); break;
        case ScreenId::Gps:        s = make<GpsScreen>(slot_); break;
        case ScreenId::Fusion:     s = make<FusionScreen>(slot_); break;
        case ScreenId::System:     s = make<SystemScreen>(slot_); break;
        case ScreenId::Power:      s = make<PowerScreen>(slot_); break;
        case ScreenId::Bus:        s = make<BusScreen>(slot_); break;
        case ScreenId::Radio:      s = make<RadioScreen>(slot_); break;
        case ScreenId::Update:     s = make<UpdateScreen>(slot_); break;
        case ScreenId::GearCal:    s = make<GearCalScreen>(slot_); break;
        case ScreenId::Config:
        case ScreenId::None:
        default: {
            auto* t = make<ConfigScreen>(slot_);
            t->bind_settings(settings_);
            if (config_resume_) {
                config_resume_ = false;
                t->resume(config_memo_);
            }
            s = t;
            id = ScreenId::Config;
            break;
        }
    }
    s->bind(ctx_);
    active_ = s;
    active_id_ = id;
}

void App::enter_current(Canvas& c) {
    const ScreenId id = screen_for(nav_.mode, nav_.screen);
    if (active_ && active_id_ == id) return;
    build(id);
    active_->enter(c);
    needs_full_ = true;
}

void App::set_trip_source(const IPointSource* s) {
    trip_src_ = s;
    if (active_id_ == ScreenId::TripMap) static_cast<TripMapScreen*>(active_)->set_source(s);
}

void App::set_track_source(const IPointSource* s) {
    track_src_ = s;
    if (active_id_ == ScreenId::TrackMap) static_cast<TrackMapScreen*>(active_)->set_source(s);
}

void App::set_lap_history(const uint32_t* laps, size_t n, uint32_t best,
                          const core::state::LapPeak* peaks) {
    laps_ = laps;
    lap_count_ = n;
    best_lap_ = best;
    lap_peaks_ = peaks;
    if (active_id_ == ScreenId::LapHistory) {
        static_cast<LapHistoryScreen*>(active_)->set_history(laps, n, best, peaks);
    }
}

void App::bind_settings(core::config::Settings* s) {
    settings_ = s;
    if (active_id_ == ScreenId::Config) static_cast<ConfigScreen*>(active_)->bind_settings(s);
}

void App::begin(Canvas& c) {
    enter_current(c);
}

void App::on_input(const core::nav::InputEvent& e, Canvas& c) {
    if (!active_) return;

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

    // A text message on the strip waits for E (or its minute). Short E
    // clears it and does nothing else; only while it is the one showing, so
    // a press meant for the screen is never taken by something hidden.
    if (toast_clearable_ && e.button == Button::E && e.press == Press::Short) {
        phone_event_.until_ms = 0;
        phone_event_.clearable = false;
        toast_clearable_ = false;
        toast_.hide();
        restore(kToastBox);
        return;
    }

    Screen& s = current();
    if (s.handle(e)) {
        const int f = s.focus_now();
        if (f >= 0) nav_.focus = static_cast<uint8_t>(f);
        return;
    }

    // DIAG is a menu, not a carousel: E on the menu opens the page under the
    // cursor, B on a page comes back to the menu with the cursor on it, and
    // B on the menu has nothing to walk to. After the page's own handle(),
    // so a page that wants E (UPDATE's actions) still has it.
    if (nav_.mode == core::nav::Mode::Diag) {
        using core::nav::Press;
        if (nav_.screen == 0 && e.button == Button::E && e.press == Press::Short) {
            // The row below the pages is the build's program, not a page.
            if (nav_.focus >= DiagMenuScreen::kPages) {
                if (ctx_.program) ctx_.program->launch();
                return;
            }
            go_screen(static_cast<uint8_t>(nav_.focus + 1), 0, c);
            return;
        }
        if (e.button == Button::B) {
            if (nav_.screen != 0) {
                go_screen(0, static_cast<uint8_t>(nav_.screen - 1), c);
            }
            return;
        }
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

void App::go_screen(uint8_t screen, uint8_t focus, Canvas& c) {
    nav_.screen = screen;
    nav_.focus = focus;
    enter_current(c);
    current().set_focus(focus);
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
        const bool grave = k == Kind::NoPulse || k == Kind::BikeDown || k == Kind::BatteryHigh;
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

namespace {

// `src` into `dst`, cut to fit on a UTF-8 boundary, ending "..." when cut:
// the toast is one line of a fixed width, and the font cuts nothing itself.
void clip(char* dst, size_t cap, const char* src) {
    size_t len = std::strlen(src);
    if (len < cap) {
        std::memcpy(dst, src, len + 1);
        return;
    }
    size_t n = cap > 4 ? cap - 4 : 0;
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, src, n);
    std::memcpy(dst + n, "...", 4);
}

void fmt_secs(char* buf, size_t n, uint32_t ms, int places) {
    core::units::fmt_float(buf, n, ms / 1000.0f, places);
}

}  // namespace

void App::ToastEvent::set(const char* b, const char* s, Color c, uint32_t until) {
    clip(big, sizeof big, b);
    clip(small, sizeof small, s);
    color = c;
    until_ms = until ? until : 1;
}

bool App::ToastEvent::live(uint32_t now) const {
    return until_ms != 0 && static_cast<int32_t>(until_ms - now) > 0;
}

void App::update_toast(const core::state::Frame& f) {
    namespace st = core::stunt;
    using P = core::state::Perf;
    const core::config::Settings* cfg = ctx_.settings;
    const bool popups = !cfg || cfg->popups;
    const bool notify_on = !cfg || cfg->notify_on;
    const uint32_t now = f.t_ms;

    // Whatever was already there at the first frame is history, not news;
    // and a counter back at 0 (a source that started over) is nothing new.
    if (!toast_seen_init_) {
        toast_seen_init_ = true;
        seen_perf_ = f.perf.seq;
        seen_hrr_ = f.hr_recovery.seq;
        seen_stunt_ = f.stunt.seq;
        seen_notify_ = f.notify.seq;
        seen_line_refused_ = f.lap.line_refused_seq;
    }

    char big[40], small[64], a[16], b[16];
    const bool metric = cfg && cfg->unit_system ==
                                   static_cast<uint8_t>(core::units::System::Metric);
    const char* dunit = metric ? "km/h" : "mph";

    // Heart-rate recovery, parked after a ride: the minute's fall, and the
    // second minute's if it stayed parked. HEART shows it itself.
    if (f.hr_recovery.seq != seen_hrr_) {
        seen_hrr_ = f.hr_recovery.seq;
        const core::state::HrRecovery& r = f.hr_recovery;
        if (r.has_drop2) {
            std::snprintf(big, sizeof big, "HR RECOVERY %+d", -static_cast<int>(r.drop2));
            std::snprintf(small, sizeof small, "bpm in 2 min, from %u", static_cast<unsigned>(r.from_bpm));
        } else {
            std::snprintf(big, sizeof big, "HR RECOVERY %+d", -static_cast<int>(r.drop1));
            std::snprintf(small, sizeof small, "bpm in 1 min, from %u%s",
                          static_cast<unsigned>(r.from_bpm),
                          r.count > 1 && r.drop1 >= r.best_drop1 ? "   BEST TODAY" : "");
        }
        if (popups && r.seq != 0 && active_id_ != ScreenId::Heart) {
            game_event_.set(big, small, color::kGood, now + kRecoveryToastMs);
        }
    }

    if (f.perf.seq != seen_perf_) {
        seen_perf_ = f.perf.seq;
        const uint8_t m = f.perf.event;
        const P::Result& r = f.perf.last[m < P::kCount ? m : 0];
        const bool best = f.perf.event_best;
        if (m == P::Brake1 || m == P::Brake2) {
            const float d = metric ? r.x * 0.3048f : r.x;
            core::units::fmt_float(a, sizeof a, d, 0);
            fmt_secs(b, sizeof b, r.ms, 2);
            std::snprintf(big, sizeof big, "%s %s %s", perf_metric_name(m, metric), a, metric ? "M" : "FT");
            std::snprintf(small, sizeof small, "%s s%s", b, best ? "   NEW BEST" : "");
        } else {
            fmt_secs(a, sizeof a, r.ms, 2);
            std::snprintf(big, sizeof big, "%s %ss", perf_metric_name(m, metric), a);
            if (m >= P::Ft60) {
                const float trap = metric ? r.x * 1.609344f : r.x;
                core::units::fmt_float(b, sizeof b, trap, 0);
                std::snprintf(small, sizeof small, "trap %s %s%s", b, dunit, best ? "   NEW BEST" : "");
            } else {
                std::snprintf(small, sizeof small, "%s", best ? "NEW BEST" : "");
            }
        }
        // Only a new best: every pull-away from a stop is a run, and a
        // toast for each would be noise. PERF lists them all, and shows them
        // there itself.
        if (popups && f.perf.seq != 0 && best && active_id_ != ScreenId::Perf) {
            game_event_.set(big, small, color::kWarn, now + kPerfToastMs);
        }
    }

    // STUNT shows the trick itself, bigger; the strip would only cover its
    // records. SPEED shows it in the lean gauge's place, as it does a jump.
    const bool stunt_toasts = popups && active_id_ != ScreenId::Stunt &&
                              active_id_ != ScreenId::Speed;
    if (f.stunt.seq != seen_stunt_) {
        seen_stunt_ = f.stunt.seq;
        const core::state::Stunt& s = f.stunt;
        if (s.kind == st::Looped) {
            std::snprintf(big, sizeof big, "LOOPED OUT!");
            std::snprintf(small, sizeof small, "no points, combo lost");
            if (stunt_toasts && s.seq != 0) game_event_.set(big, small, color::kAlarm, now + kStuntToastMs);
        } else {
            std::snprintf(big, sizeof big, "%s! +%u", st::grade_name(s.kind, s.grade),
                          static_cast<unsigned>(s.points));
            fmt_secs(a, sizeof a, s.ms, 1);
            const char* land = st::landing_name(s.landing);
            int k = std::snprintf(small, sizeof small, "%ss %d deg", a,
                                  static_cast<int>(s.peak_deg + 0.5f));
            if (land[0] && k < static_cast<int>(sizeof small)) {
                k += std::snprintf(small + k, sizeof small - k, "  %s", land);
            }
            if (s.mult_x10 > 10 && k < static_cast<int>(sizeof small)) {
                k += std::snprintf(small + k, sizeof small - k, "  x%u.%u",
                                   s.mult_x10 / 10u, s.mult_x10 % 10u);
            }
            if (s.record && k < static_cast<int>(sizeof small)) {
                std::snprintf(small + k, sizeof small - k, "  RECORD");
            }
            if (stunt_toasts && s.seq != 0) {
                game_event_.set(big, small, s.record ? color::kWarn : color::kGood,
                                now + kStuntToastMs);
            }
        }
    }

    // A Set line that `logic` refused: the answer to a press, so it shows
    // whatever Popups says.
    if (f.lap.line_refused_seq != seen_line_refused_) {
        seen_line_refused_ = f.lap.line_refused_seq;
        const float need_mph = cfg ? core::config::lap_view(*cfg).min_speed_mph : 10.0f;
        core::units::fmt_float(a, sizeof a, metric ? need_mph * 1.609344f : need_mph, 0);
        std::snprintf(small, sizeof small, "ride over %s %s, then hold E", a, dunit);
        game_event_.set("LINE NOT SET", small, color::kWarn, now + kLineToastMs);
    }

    const core::state::Notify& n = f.notify;
    const bool moving = f.motion.best_mph >= kMovingMph;
    if (n.seq != seen_notify_) {
        seen_notify_ = n.seq;
        const char* label = n.kind == 'M' ? "MSG " : n.kind == 'X' ? "MISSED " : "";
        std::snprintf(big, sizeof big, "%s%s", label, n.who);
        const bool text = n.text[0] && (!moving || (cfg && cfg->notify_text_moving));
        // The strip holds about 36 characters of the message: a glance.
        // CRUISE -> PHONE has the rest.
        clip(small, 37, text ? n.text : n.text[0] ? "stop to read" : "");
        if (notify_on && n.seq != 0 && n.kind != 'C' && n.kind != 0) {
            phone_event_.set(big, small, n.kind == 'X' ? color::kWarn : color::kAccent,
                             now + (n.kind == 'M' ? kMessageToastMs : kNotifyToastMs));
            phone_event_.clearable = n.kind == 'M';
        }
    }

    // What shows now: a ringing call over everything, then a trick under
    // way, then the newest finished event while it lasts. Never over the
    // name card or the sweep, and never under an alert.
    const bool allowed = !splash_on_ && sweep_done_ && !alert_.active();
    const bool was = toast_.active();
    bool showing_clearable = false;
    if (allowed && notify_on && core::media::ringing(n, now)) {
        std::snprintf(big, sizeof big, "CALL %s", n.who);
        toast_.show(big, "incoming call", color::kGood);
    } else if (allowed && phone_event_.live(now)) {
        // A message waits a minute, so whether its text shows is decided as
        // it shows, not when it came: stop, and it can be read.
        const char* line = phone_event_.small;
        char msg[40];
        if (phone_event_.clearable && n.kind == 'M' && n.text[0]) {
            const bool text = !moving || (cfg && cfg->notify_text_moving);
            clip(msg, 37, text ? n.text : "stop to read");
            line = msg;
        }
        toast_.show(phone_event_.big, line, phone_event_.color,
                    phone_event_.clearable ? "E: clear" : "");
        showing_clearable = phone_event_.clearable;
    } else if (allowed && stunt_toasts && f.stunt.live_kind != st::None) {
        const core::state::Stunt& s = f.stunt;
        fmt_secs(a, sizeof a, s.live_ms, 1);
        std::snprintf(big, sizeof big, "%s %ss", s.live_kind == st::Stoppie ? "STOPPIE" : "WHEELIE", a);
        int k = std::snprintf(small, sizeof small, "%d deg  +%u",
                              static_cast<int>((s.live_kind == st::Stoppie ? -s.live_deg : s.live_deg) + 0.5f),
                              static_cast<unsigned>(s.live_points * s.combo_x10 / 10.0f));
        if (s.combo_x10 > 10 && k < static_cast<int>(sizeof small)) {
            k += std::snprintf(small + k, sizeof small - k, "  x%u.%u", s.combo_x10 / 10u,
                               s.combo_x10 % 10u);
        }
        if (s.live_balance && k < static_cast<int>(sizeof small)) {
            std::snprintf(small + k, sizeof small - k, "  BALANCE!");
        }
        toast_.show(big, small, color::kAccent);
    } else if (allowed && game_event_.live(now)) {
        toast_.show(game_event_.big, game_event_.small, game_event_.color);
    } else {
        toast_.hide();
    }
    // What it covered goes back, as for the alert: that strip, not the screen.
    if (was && !toast_.active()) restore(kToastBox);
    toast_clearable_ = showing_clearable;
}

DASH_BIG_FRAME_BEGIN   // the sweep's Frame copy (1.1 kB); runs on render's 8 kB + App stack
void App::update(const core::state::Frame& f) {
    if (!active_) return;
    // STAT -> HEART's ten minutes, kept whatever screen is up.
    if (hr_history_) hr_history_->update(f.heart, f.t_ms);
    update_alert(f);
    update_slip(f);
    update_toast(f);
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
        // A whole frame, which is the only time a render loop calls render()
        // -- where the swap happens. Without it the page was never built on
        // the board: only a test that called render() itself ever saw it.
        needs_full_ = true;
        // CONFIG comes back where it was. Its memo was saved when the
        // calibration page was built in its place.
        if (!cal) config_resume_ = true;
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
DASH_BIG_FRAME_END

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
    if (toast_.active() && c.intersects(toast_.bounds)) toast_.render(c);
    if (alert_.active() && c.intersects(alert_.bounds)) alert_.render(c);
    if (dev_running_ && c.intersects(dev_tag_.bounds)) dev_tag_.render(c);
}

void App::render(Canvas& c, bool full) {
    if (!active_) return;
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
    if (toast_.active()) {
        if (c.intersects(toast_.bounds)) toast_.render(c);
        toast_.clear_dirty();
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
    if (!active_ || needs_full_ || max == 0) return 0;
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
    if (toast_.active() && toast_.dirty && n < max) out[n++] = Damage{&toast_, toast_.bounds};
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
    if (d.w != &slip_bar_ && d.w != &alert_ && d.w != &dev_tag_ && d.w != &toast_ &&
        slip_bar_.active() && d.r.intersects(slip_bar_.bounds)) {
        slip_bar_.render(c);
    }
    if (d.w != &toast_ && d.w != &alert_ && d.w != &dev_tag_ && toast_.active() &&
        d.r.intersects(toast_.bounds)) {
        toast_.render(c);
    }
    if (d.w != &alert_ && alert_.active() && d.r.intersects(alert_.bounds)) alert_.render(c);
    if (d.w != &dev_tag_ && dev_running_ && d.r.intersects(dev_tag_.bounds)) dev_tag_.render(c);
}

void App::clear_damage() {
    splash_bar_dirty_ = false;
    restore_n_ = 0;
    if (active_) {
        for (Widget* w : current().widgets()) if (w) w->clear_dirty();
    }
    slip_bar_.clear_dirty();
    toast_.clear_dirty();
    alert_.clear_dirty();
    dev_tag_.clear_dirty();
}

}  // namespace ui
