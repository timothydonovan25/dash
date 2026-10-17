// Every screen in the firmware (§7.1).
//
//   Cruise  big speed · media · trip stats · trip map · minimal
//   Track   lap timer · lap history · track map
//   Stat    all data · engine · attitude
//   Diag    sensors · gps · system · bus and I/O · radio
//   Config  the generated ConfigItem list
//
// Layout is `static constexpr Rect` at the top of each implementation, per
// §7.5. There is no layout engine and there should not be one — the whole
// layout of a screen ought to be readable in ten lines.
#pragma once
#include "ui/screen.hpp"
#include "core/state/track_buffer.hpp"

namespace ui {

// --- Cruise -------------------------------------------------------------

class SpeedScreen final : public Screen {
public:
    const char* name() const override { return "SPEED"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    // No title bar: this screen is read at speed and wants every pixel.
    bool wants_title() const override { return false; }
    // C/D/short-E: nav does nothing with them here (focus_count() is 0, and
    // short E is always free — see ui/app.hpp), so this screen claims them
    // for §11's reserved "Ctrl" characteristic: previous, next, play/pause.
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;
    // This is the screen the panel budget is decided on, so it is the one
    // worth naming: the wire-cost report is read to find out which widget
    // to make cheaper.
    const char* widget_name(size_t i) const override;

private:
    // Everything on this screen that moves is a widget, including the tach
    // bar and the lean gauge. Only widgets are tracked for redraw, so live
    // data painted as background would be refreshed on a full repaint and
    // never in between — on a partially updated panel it simply stops.
    BigNumber   speed_;
    BigNumber   rpm_;
    ShiftStrip  shift_;
    Label       gear_;
    Label       clock_;
    TachBar     bar_stem_;
    TachBar     bar_flare_;
    LeanGauge   lean_;
    Label       sats_;
    Label       hdop_;
    // The worst-off device, in two lines: the name big enough to read at a
    // glance and what is wrong with it underneath. Both blank when the bike
    // is healthy, which is nearly always.
    Label       fault_;
    Label       fault_what_;
    MediaRow    song_;
    MediaRow    album_;
    MediaRow    artist_;
    MediaProgress progress_;
    Label       odo_;
    Label       trip_;
    Widget*     list_[18] = {};
};

// The dedicated now-playing screen: title/artist/album at a size actually
// meant to be read, a big progress ring, and speed kept small beside them
// so this is still useful moving, not just parked. SpeedScreen's own media
// block stays as it is — a glance while the speed is the point — this is
// the screen for when the song is the point.
class MediaScreen final : public Screen {
public:
    const char* name() const override { return "MEDIA"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    // Same three inputs, same meaning, as SpeedScreen — one control scheme
    // for §11's Ctrl everywhere in Cruise, not a screen-specific one.
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;

private:
    BigNumber     speed_;
    AlbumArt      art_;
    MediaRow      song_;
    MediaRow      album_;
    MediaRow      artist_;
    MediaRow      lyric_;
    MediaProgress progress_;
    Widget*       list_[7] = {};
};

// Speed and the time, and nothing else. For a long road section where the
// rest of the dash is noise — no tach bar, no lean gauge, no tiles, and no
// title bar either, because a screen showing one number does not need to be
// told apart from anything.
class MinimalScreen final : public Screen {
public:
    const char* name() const override { return "MINIM"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    bool wants_title() const override { return false; }
    WidgetList widgets() override;

private:
    BigNumber speed_;
    Label     clock_;
    Widget*   list_[2] = {};
};

// Find your way back: an arrow, the distance and the bearing to one of the
// named waypoints (core::waypoint). The list has START, which sets itself at
// the start of a ride, then TRUCK, CAMP, FUEL and MARK; the row the cursor
// is on is the one the arrow points at, and it opens on START. Short E puts
// that waypoint here, long E clears it.
class NavScreen final : public Screen {
public:
    const char* name() const override { return "NAV"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;
    uint8_t focus_count() const override { return kActions; }
    void set_focus(uint8_t f) override { focus_ = f; }

private:
    static constexpr uint8_t kActions = 5;
    NavArrow arrow_;
    Label    dist_;
    Label    dist_unit_;
    Label    bearing_;
    Label    dropped_;
    Label    status_;
    Table    actions_;
    Widget*  list_[7] = {};
    uint8_t  focus_ = 0;
};

class TripScreen final : public Screen {
public:
    const char* name() const override { return "TRIP"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;

private:
    ValueBox odo_, trip_a_, trip_b_, fuel_, hours_, top_;
    Label    hint_;
    Widget*  list_[7] = {};
};

class TripMapScreen final : public Screen {
public:
    const char* name() const override { return "MAP"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;
    void set_source(const IPointSource* src) { map_.set_source(src); }

private:
    MapView  map_;
    ValueBox distance_;
    ValueBox heading_;
    ValueBox speed_;
    Widget*  list_[4] = {};
};

// --- Track --------------------------------------------------------------

class LapScreen final : public Screen {
public:
    const char* name() const override { return "LAP"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;

private:
    Label     current_;
    Label     last_;
    Label     best_;
    DeltaBar  delta_;
    ValueBox  lap_count_;
    ValueBox  speed_;
    Label     phase_;
    Widget*   list_[7] = {};
};

class LapHistoryScreen final : public Screen {
public:
    const char* name() const override { return "LAPS"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;

    // The logic task owns lap history; the screen is handed a view of it.
    void set_history(const uint32_t* laps_ms, size_t count, uint32_t best_ms);

private:
    Table           table_;
    Label           title_;
    Widget*         list_[2] = {};
    const uint32_t* laps_ = nullptr;
    size_t          lap_count_ = 0;
    uint32_t        best_ms_ = 0;
    // What the table was last built from. It only changes when a lap is
    // scored, so it is not rebuilt (and reformatted) every frame.
    bool            shown_ = false;
    size_t          shown_count_ = 0;
    uint32_t        shown_best_ = 0;
};

class TrackMapScreen final : public Screen {
public:
    const char* name() const override { return "TRACK"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;
    void set_source(const IPointSource* src) { map_.set_source(src); }

private:
    MapView  map_;
    Label    lap_;
    ValueBox to_line_;
    DeltaBar delta_;
    Widget*  list_[4] = {};
};

// --- Stat ---------------------------------------------------------------

class AllDataScreen final : public Screen {
public:
    const char* name() const override { return "ALL"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    Table   table_;
    Widget* list_[1] = {};
};

class EngineScreen final : public Screen {
public:
    const char* name() const override { return "ENGINE"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;

private:
    ValueBox  rpm_, coolant_, tps_, volts_;
    Bar       coolant_bar_, tps_bar_;
    Sparkline rpm_trace_;
    static constexpr uint32_t kRpmTraceMs = 100;
    uint32_t  rpm_trace_ms_ = 0;
    Label     ecu_;
    Widget*   list_[8] = {};
};

class AttitudeScreen final : public Screen {
public:
    const char* name() const override { return "IMU"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;

private:
    LeanIndicator lean_;
    ValueBox      pitch_, yaw_, g_lat_, g_vert_;
    ValueBox      airtime_, best_air_, land_g_;
    Widget*       list_[8] = {};
};

// --- Diag ---------------------------------------------------------------

class SensorsScreen final : public Screen {
public:
    void draw_background(Canvas& c) override;
    const char* name() const override { return "SENSORS"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    StatusDot dots_[core::state::kDevCount];
    Widget*   list_[core::state::kDevCount] = {};
};

class GpsScreen final : public Screen {
public:
    const char* name() const override { return "GPS"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    Table   table_;
    Widget* list_[1] = {};
};

class SystemScreen final : public Screen {
public:
    const char* name() const override { return "SYSTEM"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    void temp_row(char* buf, size_t n, float c, bool valid) const;
    Table   table_;
    Widget* list_[1] = {};
};

// Supply, VIN and battery health (core::battery): what the battery rests
// at, how far it sagged on the last start, and what the charging system is
// putting back.
class PowerScreen final : public Screen {
public:
    const char* name() const override { return "POWER"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    Table   table_;
    Widget* list_[1] = {};
};

class BusScreen final : public Screen {
public:
    const char* name() const override { return "BUS"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    Table   table_;
    Widget* list_[1] = {};
};

class RadioScreen final : public Screen {
public:
    const char* name() const override { return "RADIO"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    Table   table_;
    Label   note_;
    Widget* list_[2] = {};
};

// The two firmware slots and updating them (§11.7). Three actions, each a
// focusable row; everything else is read-only. Promotion is only ever done
// here, by hand, from the dev image being kept. Every action is refused by
// `ota` unless parked, and the status line says so.
class UpdateScreen final : public Screen {
public:
    const char* name() const override { return "UPDATE"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;
    uint8_t focus_count() const override { return kActions; }
    void set_focus(uint8_t f) override { focus_ = f; }

private:
    static constexpr uint8_t kActions = 4;
    Label   running_;
    Label   other_;
    Label   c6_;
    Label   status_;
    Table   actions_;
    // The hotspot: network and password big enough to type from, then
    // the address and how long until it turns itself off.
    Label   wifi_join_;
    Label   wifi_info_;
    Widget* list_[7] = {};
    uint8_t focus_ = 0;
};

// --- Config -------------------------------------------------------------

class ConfigScreen final : public Screen {
public:
    const char* name() const override { return "CONFIG"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;
    uint8_t focus_count() const override;
    void set_focus(uint8_t f) override { focus_ = f; }
    int focus_now() const override { return focus_; }

    // The screen edits the caller's Settings in place and reports changes
    // upward as ConfigSet commands, so the keeper can write NVS and every
    // module that keeps a copy can refresh.
    void bind_settings(core::config::Settings* s) { settings_ = s; }

    // Where CONFIG was: the submenu, the cursor, an edit in progress. The
    // App saves it when the gear calibration page takes CONFIG's place (and
    // destroys this screen), and hands it back to the new one.
    struct Memo {
        bool    in_group = false;
        uint8_t group = 0;
        uint8_t focus = 0;
        bool    editing = false;
    };
    Memo memo() const { return Memo{in_group_, group_, focus_, editing_}; }
    // Coming back from the gear calibration page: the next enter() puts the
    // submenu and the cursor back where `m` says instead of starting over.
    void resume(const Memo& m) {
        in_group_ = m.in_group;
        group_ = m.group;
        focus_ = m.focus;
        editing_ = m.editing;
        resume_ = true;
    }

private:
    bool                    resume_ = false;
    Table                   table_;
    Label                   hint_;
    Widget*                 list_[2] = {};
    core::config::Settings* settings_ = nullptr;
    // Two levels: the list of submenus, then one submenu's items. `group_`
    // is the submenu open, or the one to put the cursor back on.
    bool                    in_group_ = false;
    uint8_t                 group_ = 0;
    uint8_t                 focus_ = 0;
    bool                    editing_ = false;
};

// The gear calibration page, which takes over CONFIG while a run is open
// (core/gearcal). One screen, three pages by phase: the intro with what the
// learner has seen so far, one gear at a time, and the results. Its whole
// state comes from Frame::gear_cal; the buttons post commands and `logic`
// decides what they did.
class GearCalScreen final : public Screen {
public:
    const char* name() const override { return "GEAR CAL"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;

private:
    Label   head_;
    Label   status_;
    Label   detail_;
    Bar     bar_;
    Label   line_;
    Table   table_;
    Label   hint_;
    Widget* list_[7] = {};
    uint8_t phase_ = 0;
};

}  // namespace ui
