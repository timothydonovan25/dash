// Every screen in the firmware (§7.1).
//
//   Cruise  big speed · media · trip stats · trip map · minimal
//   Track   race · lap timer · lap history · track map · session
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
// The phone's last call or message (§11.1 `N`). The text only when stopped,
// unless CONFIG -> Phone -> Text moving says otherwise.
class PhoneScreen final : public Screen {
public:
    const char* name() const override { return "PHONE"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;

private:
    static constexpr int kLines = 3;
    Label   kind_;
    Label   who_;
    Label   line_[kLines];
    Label   age_;
    Label   link_;
    Widget* list_[4 + kLines] = {};
};

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
    Label     ideal_;
    DeltaBar  delta_;
    ValueBox  lap_count_;
    ValueBox  speed_;
    Label     phase_;
    Label     split_title_;
    Label     split_;
    Widget*   list_[10] = {};
};

class LapHistoryScreen final : public Screen {
public:
    const char* name() const override { return "LAPS"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;

    // The logic task owns lap history; the screen is handed a view of it.
    // `peaks` runs parallel to `laps_ms`, or is null.
    void set_history(const uint32_t* laps_ms, size_t count, uint32_t best_ms,
                     const core::state::LapPeak* peaks = nullptr);

private:
    Table           table_;
    Widget*         list_[1] = {};
    const uint32_t* laps_ = nullptr;
    const core::state::LapPeak* peaks_ = nullptr;
    size_t          lap_count_ = 0;
    uint32_t        best_ms_ = 0;
    // What the table was last built from. It only changes when a lap is
    // scored, so it is not rebuilt (and reformatted) every frame.
    bool            shown_ = false;
    size_t          shown_count_ = 0;
    uint32_t        shown_best_ = 0;
    const core::state::LapPeak* shown_peaks_ = nullptr;
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

// The screen for riding a track day: the revs across the whole top, the
// gear as big as the panel allows, the running lap and the delta, and the
// last and best laps. Nothing else -- the factory "track" layouts strip the
// screen to this, and riders say that is the right call. No title bar.
// E does what it does on LAP: hold to set the line here, tap to re-arm.
class RaceScreen final : public Screen {
public:
    const char* name() const override { return "RACE"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    bool wants_title() const override { return false; }
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;
    const char* widget_name(size_t i) const override;

private:
    RevBar   rev_;
    Label    gear_;
    Label    status_;
    Label    current_;
    DeltaBar delta_;
    Label    last_;
    Label    best_;
    Widget*  list_[7] = {};
};

// The ride so far, on one page: the laps (best, ideal, the track's record),
// how far and how long, how fast, how far over, how high. What a rider wants
// to read in the paddock, and nothing that needs reading while moving.
class SessionScreen final : public Screen {
public:
    const char* name() const override { return "SESSION"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;

private:
    static constexpr int kTiles = 9;
    void     show(int i, const char* text, bool valid);
    Label    track_;
    Label    value_[kTiles];
    Widget*  list_[1 + kTiles] = {};
};

// --- Stat ---------------------------------------------------------------

// "0-60 MPH", "1/8 MILE", "60-0 KM/H": a core::state::Perf::Metric by name,
// in the thresholds `metric` riders get.
const char* perf_metric_name(uint8_t m, bool metric);

// The performance timers (core::perf): what the timer is doing, and each
// run's last and best. E forgets the bests.
class PerfScreen final : public Screen {
public:
    const char* name() const override { return "PERF"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;
    bool handle(const core::nav::InputEvent& e) override;

private:
    static constexpr int kRows = core::state::Perf::kCount;
    Label   status_;
    Label   name_[kRows];
    Label   last_[kRows];
    Label   best_[kRows];
    Widget* list_[1 + 3 * kRows] = {};
};

// Wheelies and stoppies (core::stunt): the trick under way or the last one,
// its angle against the balance-point band, the combo, and the session's
// score and records. E starts a new session score.
class StuntScreen final : public Screen {
public:
    const char* name() const override { return "STUNT"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;
    bool handle(const core::nav::InputEvent& e) override;

private:
    static constexpr int kTiles = 6;
    Label   head_;      // WHEELIE, or LAST: SICK
    Label   time_;      // the trick's time, big
    Label   detail_;    // angle, points, landing
    Label   combo_;
    Label   score_;
    Bar     angle_;     // lift, 0..70 degrees
    Label   value_[kTiles];
    Widget* list_[6 + kTiles] = {};
};

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
    ValueBox  rpm_, hours_, tps_, volts_;
    Bar       tps_bar_;
    Sparkline rpm_trace_;
    static constexpr uint32_t kRpmTraceMs = 100;
    uint32_t  rpm_trace_ms_ = 0;
    Label     ecu_;
    Widget*   list_[7] = {};
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

// DIAG's front page: its pages as a list, like CONFIG's submenus, each with
// a one-word reading of how it stands. C and D move, E opens a page, and B
// on a page comes back here with the cursor where it was (ui::App), so a
// page eight screens along is not eight presses of B away.
class DiagMenuScreen final : public Screen {
public:
    static constexpr uint8_t kPages = 8;
    const char* name() const override { return "MENU"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    void draw_background(Canvas& c) override;
    WidgetList widgets() override;
    uint8_t focus_count() const override { return rows(); }
    void set_focus(uint8_t f) override;

private:
    // A build with a ui::IProgram has it as one more row, below the pages.
    uint8_t rows() const {
        return static_cast<uint8_t>(kPages + (has_ctx() && ctx().program ? 1 : 0));
    }

    Table   table_;
    Widget* list_[1] = {};
    uint8_t focus_ = 0;
};

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

// Sensor fusion (core::fusion, docs/fusion.md): the fused road speed and
// how sure it is, and each source beside it -- what it reads, how far that
// is from the fused speed, and whether the filter is using it, gating it
// out this step, or has called it suspect. Then the accelerometer's bias,
// the wheel sensors' verdicts, and how often the filter has had to restart.
class FusionScreen final : public Screen {
public:
    const char* name() const override { return "FUSION"; }
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

    // The screen reads Settings and never writes it: an edit goes up as a
    // ConfigSet and `logic`, the one writer, applies it (rule 2). It used
    // to edit in place from render, on the other core from logic's own
    // writes to the same table.
    void bind_settings(const core::config::Settings* s) { settings_ = s; }

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
    const core::config::Settings* settings_ = nullptr;
    // The value last posted for `pending_key_`, until Settings shows it:
    // a second press before logic has applied the first steps on from
    // here, not from the old value, or it would be lost.
    bool                    has_pending_ = false;
    uint16_t                pending_key_ = 0;
    float                   pending_val_ = 0.0f;
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
