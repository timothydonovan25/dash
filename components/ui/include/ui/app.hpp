// The screen registry and the render loop.
//
// This is the only place that knows which screens exist and which mode they
// belong to. `nav` decides where the rider is, the App decides what that
// means, and the render task calls `update()` then `render()` once a frame.
#pragma once
#include "ui/screens.hpp"
#include "core/alert/alert.hpp"
#include "core/nav/nav.hpp"

namespace ui {

class App {
public:
    explicit App(const Context& ctx);

    // Lays out the first screen. Must be called before update or render.
    void begin(Canvas& c);

    // A debounced button event from `input`. Screens get first refusal —
    // the config screen wants C and D while it is editing a value — and
    // anything they do not take goes to the navigation state machine.
    void on_input(const core::nav::InputEvent& e, Canvas& c);

    void update(const core::state::Frame& f);

    // Repaints the widgets that changed. `full` forces everything, which is
    // what a screen change and a panel reset both need.
    void render(Canvas& c, bool full = false);

    // True while a whole-screen repaint is outstanding: the first frame, a
    // screen change, or an alert being dismissed. The render task asks, and
    // clears it once it has actually pushed a full frame.
    bool needs_full_repaint() const { return needs_full_; }

    // A partial update, for when no full repaint is due. One rectangle that
    // has to go to the panel, and what draws it (null: the splash).
    struct Damage {
        Widget* w = nullptr;
        Rect    r;
    };
    static constexpr size_t kMaxDamage = 64;

    // This frame's rectangles, in order. Each is drawn with draw_damage()
    // into a canvas windowed onto it, which puts the alert and the dev tag
    // back on top wherever it overlaps them, so an overlay is resent only
    // when it changed itself. Clears nothing: call clear_damage() once all
    // of them are on the panel, or they come round again next frame.
    // The bike's render task, the simulator and test_ui all use this, so
    // what is costed, what is sent and what is checked are one thing.
    size_t damage(Damage* out, size_t max);
    void draw_damage(Canvas& c, const Damage& d);
    void clear_damage();

    // The name card at key-on, before the sweep. Whole milliseconds.
    static constexpr uint32_t kSplashMs = 1500;
    bool splashing() const { return splash_on_; }
    void clear_full_repaint() { needs_full_ = false; splash_bar_dirty_ = false; }

    Screen& current();
    const core::nav::State& nav_state() const { return nav_; }
    const char* mode_name() const { return core::nav::mode_name(nav_.mode); }

    AlertOverlay& alert() { return alert_; }
    // Which alert the overlay is showing, None if it is down or dismissed.
    core::alert::Kind alert_kind() const { return alert_kind_; }
    SlipBar& slip_bar() { return slip_bar_; }

    // Wiring for the things a screen reads but does not own.
    void set_trip_source(const IPointSource* s)  { trip_map_.set_source(s); }
    void set_track_source(const IPointSource* s) { track_map_.set_source(s); }
    void set_lap_history(const uint32_t* laps, size_t n, uint32_t best) {
        lap_history_.set_history(laps, n, best);
    }
    void bind_settings(core::config::Settings* s) { config_.bind_settings(s); }

    // Screen count per mode, for tests and for the diag dump.
    const core::nav::Layout& layout() const { return layout_; }

private:
    Screen* screen_for(core::nav::Mode m, uint8_t index);
    void    enter_current(Canvas& c);

    const Context&    ctx_;
    core::nav::State  nav_{};
    core::nav::Layout layout_{};

    SpeedScreen      speed_;
    MediaScreen      media_;
    MinimalScreen    minimal_;
    TripScreen       trip_;
    NavScreen        nav_screen_;
    TripMapScreen    trip_map_;
    LapScreen        lap_;
    LapHistoryScreen lap_history_;
    TrackMapScreen   track_map_;
    AllDataScreen    all_data_;
    EngineScreen     engine_;
    AttitudeScreen   attitude_;
    SensorsScreen    sensors_;
    GpsScreen        gps_;
    SystemScreen     system_;
    PowerScreen      power_;
    BusScreen        bus_;
    RadioScreen      radio_;
    UpdateScreen     update_;
    ConfigScreen     config_;
    GearCalScreen    gearcal_;

    AlertOverlay     alert_;
    // Decides what goes in alert_; see update_alert().
    core::alert::Alerter alerter_;
    core::alert::Kind    alert_kind_ = core::alert::Kind::None;
    void             update_alert(const core::state::Frame& f);

    // Wheelspin and lockup (core/slip), over the top of every screen.
    SlipBar              slip_bar_;
    core::slip::Meter    slip_meter_;
    void                 update_slip(const core::state::Frame& f);

    // A dev image (§11.7) wears this on every screen.
    DevTag           dev_tag_;
    bool             dev_running_ = false;
    Screen*          entered_ = nullptr;
    bool             needs_full_ = true;
    // CONFIG shows the gear calibration page instead while a run is open.
    bool             cal_active_ = false;
    bool             cal_swap_ = false;

    // The start-up gauge sweep: when the first frame arrived, and whether
    // the sweep has finished for good.
    bool             splash_started_ = false;
    bool             splash_on_ = false;
    uint32_t         splash_start_ms_ = 0;
    uint16_t         splash_permille_ = 0;
    bool             splash_bar_dirty_ = false;
    char             splash_ver_[32] = {};
    void             render_splash(Canvas& c);

    bool             sweep_started_ = false;
    bool             sweep_done_ = false;
    uint32_t         sweep_start_ms_ = 0;
};

}  // namespace ui
