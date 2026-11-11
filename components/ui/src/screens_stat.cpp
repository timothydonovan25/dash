#include "ui/screens.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace ui {

using core::state::Dev;
using core::state::Frame;
namespace u = core::units;

namespace {
// Shared by the table screens: write "<value> <unit>" into a row, or "--"
// when the device behind it is not Ok. This is the §12 rule, and putting it
// in one helper is what stops one screen quietly forgetting it.
void row_value(char* buf, size_t n, float v, int decimals, const char* unit,
               bool valid) {
    size_t k = u::fmt_value(buf, n, v, decimals, valid);
    if (valid && unit && unit[0] && k + 2 < n) {
        buf[k++] = ' ';
        for (const char* p = unit; *p && k + 1 < n; ++p) buf[k++] = *p;
        buf[k] = '\0';
    }
}
}  // namespace

// --- AllDataScreen ------------------------------------------------------
namespace {
constexpr Rect kAllTable {14, 40, 452, 264};
}  // namespace

void AllDataScreen::enter(Canvas&) {
    table_.set_bounds(kAllTable);
    table_.configure(10, 2);
    list_[0] = &table_;
}

void AllDataScreen::update(const Frame& f) {
    char v[24];
    const char* unit = "mph";

    row_value(v, sizeof v, u::speed_for_display(f.motion.best_mph, units(), &unit),
              0, unit, f.health.working(Dev::Gnss));
    table_.set_row(0, "Speed", v);

    row_value(v, sizeof v, u::rpm_for_display(f.engine.rpm), 0, "rpm", f.engine.rpm > 0.0f);
    table_.set_row(1, "RPM", v);

    // Air-cooled: no coolant temperature. Engine hours are what a dirt
    // bike's maintenance is counted in.
    row_value(v, sizeof v, f.trip.eng_hours, 1, "h", true);
    table_.set_row(2, "Eng hours", v);

    row_value(v, sizeof v, f.att.lean_deg, 0, "deg", f.health.ok(Dev::Imu));
    table_.set_row(3, "Lean", v);

    unit = "mi";
    row_value(v, sizeof v, u::distance_for_display(f.trip.trip_a_mi, units(), &unit),
              1, unit, true);
    table_.set_row(4, "Trip A", v);

    u::fmt_duration(v, sizeof v, f.lap.best_ms, f.lap.best_ms > 0);
    table_.set_row(5, "Best lap", v);

    row_value(v, sizeof v, f.elec.vbat, 1, "V", f.elec.vbat > 0.0f);
    table_.set_row(6, "Battery", v);

    // TPMS comes over BLE, and the scan is not written yet. The wheels read `--`
    // rather than 0 psi — a zero here would be indistinguishable from a
    // tyre that has actually gone flat.
    unit = "psi";
    row_value(v, sizeof v, u::pressure_for_display(f.tpms.front.psi, units(), &unit),
              1, unit, f.tpms.front.valid);
    table_.set_row(7, "Front tyre", v);

    row_value(v, sizeof v, u::pressure_for_display(f.tpms.rear.psi, units(), &unit),
              1, unit, f.tpms.rear.valid);
    table_.set_row(8, "Rear tyre", v);

    row_value(v, sizeof v, f.att.airtime_ms / 1000.0f, 2, "s",
              f.health.ok(Dev::Imu) && f.att.airtime_ms > 0);
    table_.set_row(9, "Last air", v);

    table_.set_row_count(10);
}

WidgetList AllDataScreen::widgets() { return WidgetList{list_, 1}; }

// --- EngineScreen -------------------------------------------------------
namespace {
// rpm and its trace across the top, the three secondary readings below.
// The bike is air-cooled, so there is no coolant tile: engine hours instead.
constexpr Rect kEngEcu     { 14,   6, 452,  14};
constexpr Rect kEngRpm     { 14,  32, 190,  80};
constexpr Rect kEngTrace   {216,  32, 250,  80};
constexpr Rect kEngHours   { 14, 138, 144,  76};
constexpr Rect kEngTps     {168, 138, 144,  76};
constexpr Rect kEngTpsBar  {168, 226, 144,  14};
constexpr Rect kEngVolts   {322, 138, 144,  76};
}  // namespace

void EngineScreen::enter(Canvas&) {
    ecu_.set_bounds(kEngEcu);
    rpm_.set_bounds(kEngRpm.inset(8));   rpm_.configure("RPM", "", 0, 5);
    rpm_trace_.set_bounds(kEngTrace); rpm_trace_.configure(0.0f, 11000.0f);
    hours_.set_bounds(kEngHours.inset(7)); hours_.configure("ENG HOURS", "h", 1, 3);
    tps_.set_bounds(kEngTps.inset(7)); tps_.configure("THROTTLE", "%", 0, 3);
    tps_bar_.set_bounds(kEngTpsBar);
    tps_bar_.configure(color::kGood, color::kGood, 2.0f);
    volts_.set_bounds(kEngVolts.inset(7)); volts_.configure("BATTERY", "V", 1, 3);

    list_[0] = &ecu_;     list_[1] = &rpm_;      list_[2] = &rpm_trace_;
    list_[3] = &hours_;
    list_[4] = &tps_;     list_[5] = &tps_bar_;  list_[6] = &volts_;
}

void EngineScreen::update(const Frame& f) {
    // The ECU comes by radio from the K-line adapter (§10.1), which most
    // of the time is not there, or is there with the key off. This line
    // says which, and carries what the ECU has that no tile does.
    char ecu[48];
    if (f.engine.ecu_online) {
        const char* unit = "";
        const float eng = u::temp_for_display(f.engine.engine_c, units(), &unit);
        const float air = u::temp_for_display(f.engine.intake_c, units(), &unit);
        std::snprintf(ecu, sizeof ecu, "ECU  engine %.0f %s  air %.0f %s  %.0f kPa", eng, unit,
                      air, unit, f.engine.map_kpa);
    } else {
        std::snprintf(ecu, sizeof ecu, "%s", f.engine.ecu_adapter
                          ? "ECU SILENT  -  adapter heard, no data"
                          : "ECU OFFLINE  -  no adapter (CONFIG -> ECU)");
    }
    ecu_.configure(ecu, f.engine.ecu_online ? color::kGood : color::kDim, 1, Align::Left);

    // RPM comes from the inductive pickup, not the ECU, and stays available
    // whatever the adapter is doing.
    const bool have_rpm = f.engine.rpm > 0.0f || f.engine.neutral;
    rpm_.set(u::rpm_for_display(f.engine.rpm), have_rpm);
    // 10 Hz, not every frame: 64 points is then ~6 s of history rather than
    // one, and the 250x80 trace is sent a tenth as often.
    if (have_rpm && f.t_ms - rpm_trace_ms_ >= kRpmTraceMs) {
        rpm_trace_ms_ = f.t_ms;
        rpm_trace_.push(f.engine.rpm);
    }

    hours_.set(f.trip.eng_hours, true);

    tps_.set(f.engine.tps_pct, f.engine.ecu_online);
    tps_bar_.set(f.engine.tps_pct / 100.0f, f.engine.ecu_online);

    volts_.set(f.elec.vbat, f.elec.vbat > 0.0f);
}

void EngineScreen::draw_background(Canvas& c) {
    for (const Rect& r : {kEngRpm, kEngHours, kEngTps, kEngVolts}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
}

WidgetList EngineScreen::widgets() { return WidgetList{list_, 7}; }

// --- AttitudeScreen -----------------------------------------------------
namespace {
// The lean gauge is the point of this screen, so it gets a square of its
// own on the left and the numbers go in a grid beside it.
constexpr Rect kAttLean  { 14,  34, 230, 230};
constexpr Rect kAttPitch {258,  34, 100,  64};
constexpr Rect kAttYaw   {366,  34, 100,  64};
constexpr Rect kAttGlat  {258, 110, 100,  64};
constexpr Rect kAttGvert {366, 110, 100,  64};
constexpr Rect kAttAir   {258, 186, 100,  64};
constexpr Rect kAttBest  {366, 186, 100,  64};
constexpr Rect kAttLand  {258, 262, 100,  46};
}  // namespace

void AttitudeScreen::enter(Canvas&) {
    lean_.set_bounds(kAttLean.inset(6));
    pitch_.set_bounds(kAttPitch.inset(6));  pitch_.configure("PITCH", "deg", 0, 2);
    yaw_.set_bounds(kAttYaw.inset(6));      yaw_.configure("YAW", "d/s", 0, 2);
    g_lat_.set_bounds(kAttGlat.inset(6));   g_lat_.configure("LAT G", "g", 2, 2);
    g_vert_.set_bounds(kAttGvert.inset(6)); g_vert_.configure("VERT G", "g", 2, 2);
    airtime_.set_bounds(kAttAir.inset(6));  airtime_.configure("AIRTIME", "s", 2, 2);
    best_air_.set_bounds(kAttBest.inset(6));best_air_.configure("BEST AIR", "s", 2, 2);
    land_g_.set_bounds(kAttLand.inset(6));  land_g_.configure("LANDING", "g", 1, 2);

    list_[0] = &lean_;    list_[1] = &pitch_;   list_[2] = &yaw_;
    list_[3] = &g_lat_;   list_[4] = &g_vert_;  list_[5] = &airtime_;
    list_[6] = &best_air_; list_[7] = &land_g_;
}

void AttitudeScreen::update(const Frame& f) {
    const bool imu = f.health.ok(Dev::Imu);
    lean_.set(f.att.lean_deg, imu);
    lean_.set_peaks(f.lap.max_lean_left_deg, f.lap.max_lean_right_deg);
    pitch_.set(f.att.pitch_deg, imu);
    yaw_.set(f.att.yaw_rate_dps, imu);
    g_lat_.set(f.att.g_lat, imu);
    g_vert_.set(f.att.g_vert, imu);
    airtime_.set(f.att.airtime_ms / 1000.0f, imu && f.att.airtime_ms > 0);
    // The session's longest (core::session, from `logic`).
    best_air_.set(f.session.best_air_ms / 1000.0f, imu && f.session.best_air_ms > 0);
    land_g_.set(f.att.last_land_g, imu && f.att.last_land_g > 0.0f);
}

bool AttitudeScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    if (e.button == Button::E && e.press == Press::Long) {
        // Park upright, hold E. Without this every lean reading carries the
        // fixed bias of however the board ended up bolted in (§8).
        // Peaks taken against the old mount offset are not records.
        post(core::state::Command(core::state::CommandId::LeanPeaksReset));
        return post(core::state::Command(core::state::CommandId::ImuCalibrate));
    }
    if (e.button == Button::E) {
        // The peaks live in `logic` (Lap::max_lean_*), so it resets them.
        return post(core::state::Command(core::state::CommandId::LeanPeaksReset));
    }
    return false;
}

void AttitudeScreen::draw_background(Canvas& c) {
    c.draw_round_rect(kAttLean, chrome::kRadius, theme::rule);
    for (const Rect& r : {kAttPitch, kAttYaw, kAttGlat, kAttGvert, kAttAir,
                          kAttBest, kAttLand}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
    c.draw_text(366, 274, "hold E to", theme::secondary, 1, Align::Left);
    c.draw_text(366, 286, "level the IMU", theme::secondary, 1, Align::Left);
}

WidgetList AttitudeScreen::widgets() { return WidgetList{list_, 8}; }

}  // namespace ui
