// The screen registry and the render loop.
//
// This is the only place that knows which screens exist and which mode they
// belong to. `nav` decides where the rider is, the App decides what that
// means, and the render task calls `update()` then `render()` once a frame.
#pragma once
#include "ui/screens.hpp"
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
    void clear_full_repaint() { needs_full_ = false; }

    Screen& current();
    const core::nav::State& nav_state() const { return nav_; }
    const char* mode_name() const { return core::nav::mode_name(nav_.mode); }

    AlertOverlay& alert() { return alert_; }

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
    BusScreen        bus_;
    RadioScreen      radio_;
    ConfigScreen     config_;

    AlertOverlay     alert_;
    Screen*          entered_ = nullptr;
    bool             needs_full_ = true;
};

}  // namespace ui
