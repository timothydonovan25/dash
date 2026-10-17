// The screen registry and the render loop.
//
// This is the only place that knows which screens exist and which mode they
// belong to. `nav` decides where the rider is, the App decides what that
// means, and the render task calls `update()` then `render()` once a frame.
#pragma once
#include <algorithm>
#include <cstddef>
#include "ui/screens.hpp"
#include "core/alert/alert.hpp"
#include "core/nav/nav.hpp"

namespace ui {

// Every screen type, in one list: the App keeps only the active screen
// alive, in storage sized and aligned for the largest of these.
template <typename... T>
struct ScreenStorage {
    static constexpr size_t size  = std::max({sizeof(T)...});
    static constexpr size_t align = std::max({alignof(T)...});
};
using AllScreens = ScreenStorage<
    SpeedScreen, MediaScreen, MinimalScreen, TripScreen, NavScreen, TripMapScreen,
    LapScreen, LapHistoryScreen, TrackMapScreen, AllDataScreen, EngineScreen,
    AttitudeScreen, SensorsScreen, GpsScreen, SystemScreen, PowerScreen, BusScreen,
    RadioScreen, UpdateScreen, ConfigScreen, GearCalScreen>;

// Which screen is alive. Order follows the modes, but nothing depends on it.
enum class ScreenId : uint8_t {
    None,
    Speed, Media, Trip, TripMap, Nav, Minimal,
    Lap, LapHistory, TrackMap,
    AllData, Engine, Attitude,
    Sensors, Gps, System, Power, Bus, Radio, Update,
    Config, GearCal,
};

class App {
public:
    explicit App(const Context& ctx);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

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
    // screen change, a dev image starting. The render task asks, and clears
    // it once it has actually pushed a full frame. A dismissed alert or a
    // cleared slip bar is not one: that is a region in damage().
    bool needs_full_repaint() const { return needs_full_; }

    // The panel's contents are no longer known (it was reset): the next
    // frame goes out whole.
    void invalidate() { needs_full_ = true; }

    // A partial update, for when no full repaint is due. One rectangle that
    // has to go to the panel, and what draws it (null: the splash).
    struct Damage {
        Widget* w = nullptr;
        Rect    r;
        // A patch of screen whose contents are unknown, painted from the
        // ground up: background, chrome, every widget under it, overlays.
        // What an overlay that has just gone away leaves behind.
        bool    region = false;
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
    void clear_full_repaint() {
        needs_full_ = false;
        splash_bar_dirty_ = false;
        restore_n_ = 0;
    }

    // The screen on the panel. Only one screen exists at a time: a screen is
    // built when it is entered and destroyed when it is left, so a reference
    // to it does not survive a screen change, and anything a screen keeps
    // between visits (a cursor, a trace) starts again. Not before begin().
    Screen& current() { return *active_; }
    ScreenId current_id() const { return active_id_; }
    const core::nav::State& nav_state() const { return nav_; }
    const char* mode_name() const { return core::nav::mode_name(nav_.mode); }

    AlertOverlay& alert() { return alert_; }
    // Which alert the overlay is showing, None if it is down or dismissed.
    core::alert::Kind alert_kind() const { return alert_kind_; }
    SlipBar& slip_bar() { return slip_bar_; }

    // Wiring for the things a screen reads but does not own. The App keeps
    // it and hands it to the screen each time that screen is built.
    void set_trip_source(const IPointSource* s);
    void set_track_source(const IPointSource* s);
    void set_lap_history(const uint32_t* laps, size_t n, uint32_t best);
    void bind_settings(core::config::Settings* s);

    // Screen count per mode, for tests and for the diag dump.
    const core::nav::Layout& layout() const { return layout_; }

private:
    ScreenId screen_for(core::nav::Mode m, uint8_t index) const;
    void     enter_current(Canvas& c);
    // Destroys the active screen, if any, and builds `id` in its place.
    void     build(ScreenId id);
    void     destroy_active();

    const Context&    ctx_;
    core::nav::State  nav_{};
    core::nav::Layout layout_{};

    // The one live screen. 21 screens held at once were 12.9 kB of the
    // render task's stack, and every new screen added to it; one at a time
    // is the largest screen, whatever the count.
    alignas(AllScreens::align) unsigned char slot_[AllScreens::size];
    Screen*          active_ = nullptr;
    ScreenId         active_id_ = ScreenId::None;

    // What the screens are handed when they are built.
    const IPointSource*     trip_src_ = nullptr;
    const IPointSource*     track_src_ = nullptr;
    const uint32_t*         laps_ = nullptr;
    size_t                  lap_count_ = 0;
    uint32_t                best_lap_ = 0;
    core::config::Settings* settings_ = nullptr;
    // CONFIG's submenu and cursor, kept while the gear calibration page has
    // its place, and given back when CONFIG is built again.
    ConfigScreen::Memo      config_memo_{};
    bool                    config_resume_ = false;

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

    // Patches of screen an overlay has just uncovered (the slip bar, an
    // alert), repainted as damage rather than as a whole frame: the slip bar
    // is 2.9 ms of wire and the alert box 6.7, a full repaint 31.
    static constexpr int kMaxRestore = 2;
    Rect             restore_[kMaxRestore] = {};
    int              restore_n_ = 0;
    void             restore(const Rect& r);
    void             paint_region(Canvas& c);

    bool             sweep_started_ = false;
    bool             sweep_done_ = false;
    uint32_t         sweep_start_ms_ = 0;
};

}  // namespace ui
