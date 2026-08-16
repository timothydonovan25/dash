#include "ui/screens.hpp"
#include "core/units/units.hpp"
#include <cmath>

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
constexpr Rect kAllTable {16, 30, 288, 430};
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
              0, unit, f.health.ok(Dev::Gnss));
    table_.set_row(0, "Speed", v);

    row_value(v, sizeof v, f.engine.rpm, 0, "rpm", f.engine.rpm > 0.0f);
    table_.set_row(1, "RPM", v);

    unit = "C";
    row_value(v, sizeof v, u::temp_for_display(f.engine.coolant_c, units(), &unit),
              0, unit, f.engine.ecu_online);
    table_.set_row(2, "Coolant", v);

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

    // TPMS comes over BLE, which is not in this build. The wheels read `--`
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
constexpr Rect kEngEcu     { 16,  24, 288,  16};
constexpr Rect kEngRpm     { 16,  50, 288,  60};
constexpr Rect kEngTrace   { 16, 120, 288,  80};
constexpr Rect kEngCoolant { 16, 215, 140,  60};
constexpr Rect kEngCoolBar { 16, 275, 140,  14};
constexpr Rect kEngTps     {166, 215, 140,  60};
constexpr Rect kEngTpsBar  {166, 275, 140,  14};
constexpr Rect kEngVolts   { 16, 320, 140,  60};
}  // namespace

void EngineScreen::enter(Canvas&) {
    ecu_.set_bounds(kEngEcu);
    rpm_.set_bounds(kEngRpm);        rpm_.configure("RPM", "", 0, 5);
    rpm_trace_.set_bounds(kEngTrace); rpm_trace_.configure(0.0f, 11000.0f);
    coolant_.set_bounds(kEngCoolant); coolant_.configure("COOLANT", "C", 0, 3);
    coolant_bar_.set_bounds(kEngCoolBar);
    coolant_bar_.configure(color::kAccent, color::kAlarm, 0.85f);
    tps_.set_bounds(kEngTps);         tps_.configure("THROTTLE", "%", 0, 3);
    tps_bar_.set_bounds(kEngTpsBar);
    tps_bar_.configure(color::kGood, color::kGood, 2.0f);
    volts_.set_bounds(kEngVolts);     volts_.configure("BATTERY", "V", 1, 3);

    list_[0] = &ecu_;     list_[1] = &rpm_;      list_[2] = &rpm_trace_;
    list_[3] = &coolant_; list_[4] = &coolant_bar_;
    list_[5] = &tps_;     list_[6] = &tps_bar_;  list_[7] = &volts_;
}

void EngineScreen::update(const Frame& f) {
    // The ECU is not reachable on this hardware: the board has no CAN
    // transceiver and the K-line adapter cannot be rewired without a respin
    // (§10.1). NullEngine is what ships, so this says so plainly instead of
    // showing a screen full of zeros.
    ecu_.configure(f.engine.ecu_online ? "ECU ONLINE" : "ECU OFFLINE  -  no adapter",
                   f.engine.ecu_online ? color::kGood : color::kDim, 1, Align::Left);

    // RPM comes from the inductive pickup, not the ECU, and stays available
    // whatever the adapter is doing.
    const bool have_rpm = f.engine.rpm > 0.0f || f.engine.neutral;
    rpm_.set(f.engine.rpm, have_rpm);
    if (have_rpm) rpm_trace_.push(f.engine.rpm);

    const char* tunit = "C";
    coolant_.set(u::temp_for_display(f.engine.coolant_c, units(), &tunit),
                 f.engine.ecu_online);
    coolant_.set_unit(tunit);
    coolant_bar_.set(f.engine.coolant_c / 120.0f, f.engine.ecu_online);

    tps_.set(f.engine.tps_pct, f.engine.ecu_online);
    tps_bar_.set(f.engine.tps_pct / 100.0f, f.engine.ecu_online);

    volts_.set(f.elec.vbat, f.elec.vbat > 0.0f);
}

WidgetList EngineScreen::widgets() { return WidgetList{list_, 8}; }

// --- AttitudeScreen -----------------------------------------------------
namespace {
constexpr Rect kAttLean  { 60,  30, 200, 200};
constexpr Rect kAttPitch { 16, 245, 140,  55};
constexpr Rect kAttYaw   {166, 245, 140,  55};
constexpr Rect kAttGlat  { 16, 305, 140,  55};
constexpr Rect kAttGvert {166, 305, 140,  55};
constexpr Rect kAttAir   { 16, 365, 140,  55};
constexpr Rect kAttBest  {166, 365, 140,  55};
constexpr Rect kAttLand  { 16, 425, 140,  55};
}  // namespace

void AttitudeScreen::enter(Canvas&) {
    lean_.set_bounds(kAttLean);
    pitch_.set_bounds(kAttPitch);    pitch_.configure("PITCH", "deg", 0, 3);
    yaw_.set_bounds(kAttYaw);        yaw_.configure("YAW RATE", "d/s", 0, 3);
    g_lat_.set_bounds(kAttGlat);     g_lat_.configure("LAT G", "g", 2, 3);
    g_vert_.set_bounds(kAttGvert);   g_vert_.configure("VERT G", "g", 2, 3);
    airtime_.set_bounds(kAttAir);    airtime_.configure("AIRTIME", "s", 2, 3);
    best_air_.set_bounds(kAttBest);  best_air_.configure("BEST AIR", "s", 2, 3);
    land_g_.set_bounds(kAttLand);    land_g_.configure("LANDING", "g", 1, 3);

    list_[0] = &lean_;    list_[1] = &pitch_;   list_[2] = &yaw_;
    list_[3] = &g_lat_;   list_[4] = &g_vert_;  list_[5] = &airtime_;
    list_[6] = &best_air_; list_[7] = &land_g_;
}

void AttitudeScreen::update(const Frame& f) {
    const bool imu = f.health.ok(Dev::Imu);
    lean_.set(f.att.lean_deg, imu);
    pitch_.set(f.att.pitch_deg, imu);
    yaw_.set(f.att.yaw_rate_dps, imu);
    g_lat_.set(f.att.g_lat, imu);
    g_vert_.set(f.att.g_vert, imu);
    airtime_.set(f.att.airtime_ms / 1000.0f, imu && f.att.airtime_ms > 0);
    best_air_.set(0.0f, false);          // owned by the logic task, not the Frame
    land_g_.set(f.att.last_land_g, imu && f.att.last_land_g > 0.0f);
}

bool AttitudeScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    if (e.button == Button::E && e.press == Press::Long) {
        // Park upright, hold E. Without this every lean reading carries the
        // fixed bias of however the board ended up bolted in (§8).
        lean_.reset_peaks();
        return post(core::state::Command(core::state::CommandId::ImuCalibrate));
    }
    if (e.button == Button::E) {
        lean_.reset_peaks();
        return true;
    }
    return false;
}

WidgetList AttitudeScreen::widgets() { return WidgetList{list_, 8}; }

}  // namespace ui
