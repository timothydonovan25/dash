// Every screen in the firmware (§7.1).
//
//   Cruise  big speed · trip stats · trip map
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
    WidgetList widgets() override;

private:
    BigNumber  speed_;
    ShiftStrip shift_;
    ValueBox   rpm_;
    ValueBox   gear_hint_;
    Label      clock_;
    Label      neutral_;
    Widget*    list_[6] = {};
};

class TripScreen final : public Screen {
public:
    const char* name() const override { return "TRIP"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
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
    WidgetList widgets() override;
    void set_source(const IPointSource* src) { map_.set_source(src); }

private:
    MapView  map_;
    ValueBox distance_;
    ValueBox heading_;
    Widget*  list_[3] = {};
};

// --- Track --------------------------------------------------------------

class LapScreen final : public Screen {
public:
    const char* name() const override { return "LAP"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
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
};

class TrackMapScreen final : public Screen {
public:
    const char* name() const override { return "TRACK"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    WidgetList widgets() override;
    void set_source(const IPointSource* src) { map_.set_source(src); }

private:
    MapView  map_;
    Label    lap_;
    ValueBox to_line_;
    Widget*  list_[3] = {};
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
    WidgetList widgets() override;

private:
    ValueBox  rpm_, coolant_, tps_, volts_;
    Bar       coolant_bar_, tps_bar_;
    Sparkline rpm_trace_;
    Label     ecu_;
    Widget*   list_[8] = {};
};

class AttitudeScreen final : public Screen {
public:
    const char* name() const override { return "IMU"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
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

// --- Config -------------------------------------------------------------

class ConfigScreen final : public Screen {
public:
    const char* name() const override { return "CONFIG"; }
    void enter(Canvas& c) override;
    void update(const core::state::Frame& f) override;
    bool handle(const core::nav::InputEvent& e) override;
    WidgetList widgets() override;
    uint8_t focus_count() const override;
    void set_focus(uint8_t f) override { focus_ = f; }

    // The screen edits the caller's Settings in place and reports changes
    // upward as ConfigSet commands, so the keeper can write NVS and every
    // module that keeps a copy can refresh.
    void bind_settings(core::config::Settings* s) { settings_ = s; }

private:
    Table                   table_;
    Label                   hint_;
    Widget*                 list_[2] = {};
    core::config::Settings* settings_ = nullptr;
    uint8_t                 focus_ = 0;
    bool                    editing_ = false;
};

}  // namespace ui
