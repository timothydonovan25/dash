#include "ui/screens.hpp"
#include "core/units/units.hpp"

namespace ui {

using core::state::Frame;
using core::state::Dev;
namespace u = core::units;

// --- SpeedScreen --------------------------------------------------------
//
// The screen the bike spends its life on. One number, big enough to read
// without looking, and the shift lights mirrored so the strip on the bar and
// the strip on the screen can never disagree.
namespace {
constexpr Rect kSpeedNum   {  0,  60, 320, 200};
constexpr Rect kSpeedShift { 16,  20, 288,  24};
constexpr Rect kSpeedRpm   { 16, 290, 140,  50};
constexpr Rect kSpeedGear  {176, 290, 128,  50};
constexpr Rect kSpeedClock {304, 440,  16,  10};
constexpr Rect kSpeedNeutral{16, 440, 120,  16};
}  // namespace

void SpeedScreen::enter(Canvas&) {
    speed_.set_bounds(kSpeedNum);
    speed_.configure("mph", 0, 9);

    shift_.set_bounds(kSpeedShift);

    rpm_.set_bounds(kSpeedRpm);
    rpm_.configure("RPM", "", 0, 3);

    gear_hint_.set_bounds(kSpeedGear);
    gear_hint_.configure("COOLANT", "C", 0, 3);

    clock_.set_bounds(kSpeedClock);
    clock_.configure("--:--", color::kDim, 1, Align::Right);

    neutral_.set_bounds(kSpeedNeutral);
    neutral_.configure("", color::kGood, 2, Align::Left);

    list_[0] = &speed_;
    list_[1] = &shift_;
    list_[2] = &rpm_;
    list_[3] = &gear_hint_;
    list_[4] = &clock_;
    list_[5] = &neutral_;
}

void SpeedScreen::update(const Frame& f) {
    const char* unit = "mph";
    const float shown = u::speed_for_display(f.motion.best_mph, units(), &unit);
    speed_.configure(unit, 0, 9);
    // Speed comes from the wheel sensors or GPS; if neither is healthy the
    // number is not "0", it is unknown.
    const bool speed_ok = f.health.ok(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(shown, speed_ok);

    rpm_.set(f.engine.rpm, f.engine.rpm > 0.0f || f.engine.neutral);

    // Straight off the mask ctrl sent to the lamps, so the strip on the
    // screen and the strip on the handlebar cannot disagree.
    shift_.set(f.engine.shift_mask, f.engine.shift_flash);

    const char* tunit = "C";
    const float coolant = u::temp_for_display(f.engine.coolant_c, units(), &tunit);
    gear_hint_.set_unit(tunit);
    gear_hint_.set(coolant, f.engine.ecu_online);

    char clock[8];
    u::fmt_clock(clock, sizeof clock, f.gnss.utc_ms,
                 has_ctx() && ctx().settings ? ctx().settings->tz_offset_h : 0,
                 f.gnss.fix);
    clock_.set_text(clock);

    neutral_.set_text(f.engine.neutral ? "N" : "");
}

WidgetList SpeedScreen::widgets() { return WidgetList{list_, 6}; }

// --- TripScreen ---------------------------------------------------------
namespace {
constexpr Rect kTripOdo  { 16,  40, 288, 60};
constexpr Rect kTripA    { 16, 120, 140, 60};
constexpr Rect kTripB    {166, 120, 140, 60};
constexpr Rect kTripFuel { 16, 200, 140, 60};
constexpr Rect kTripHours{166, 200, 140, 60};
constexpr Rect kTripTop  { 16, 280, 140, 60};
constexpr Rect kTripHint { 16, 440, 288, 16};
}  // namespace

void TripScreen::enter(Canvas&) {
    odo_.set_bounds(kTripOdo);     odo_.configure("ODOMETER", "mi", 1, 4);
    trip_a_.set_bounds(kTripA);    trip_a_.configure("TRIP A", "mi", 1, 3);
    trip_b_.set_bounds(kTripB);    trip_b_.configure("TRIP B", "mi", 1, 3);
    fuel_.set_bounds(kTripFuel);   fuel_.configure("FUEL EST", "gal", 2, 3);
    hours_.set_bounds(kTripHours); hours_.configure("ENGINE", "h", 1, 3);
    top_.set_bounds(kTripTop);     top_.configure("TOP", "mph", 0, 3);
    hint_.set_bounds(kTripHint);
    hint_.configure("E: reset trip A", color::kDim, 1, Align::Left);

    list_[0] = &odo_;  list_[1] = &trip_a_; list_[2] = &trip_b_;
    list_[3] = &fuel_; list_[4] = &hours_;  list_[5] = &top_;
    list_[6] = &hint_;
}

void TripScreen::update(const Frame& f) {
    const char* dunit = "mi";
    odo_.set_unit(dunit);
    odo_.set(u::distance_for_display(f.trip.odo_mi, units(), &dunit), true);
    odo_.set_unit(dunit);
    trip_a_.set_unit(dunit);
    trip_a_.set(u::distance_for_display(f.trip.trip_a_mi, units(), &dunit), true);
    trip_b_.set_unit(dunit);
    trip_b_.set(u::distance_for_display(f.trip.trip_b_mi, units(), &dunit), true);

    const char* vunit = "gal";
    fuel_.set(u::volume_for_display(f.trip.fuel_used_gal, units(), &vunit), true);
    fuel_.set_unit(vunit);

    hours_.set(f.trip.eng_hours, true);

    const char* sunit = "mph";
    top_.set(u::speed_for_display(f.trip.top_mph, units(), &sunit), true);
    top_.set_unit(sunit);
}

bool TripScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    if (e.button == Button::E && e.press == Press::Long) {
        // Long press, not short: resetting a trip meter by accident on a
        // screen the rider stares at is exactly the kind of thing that
        // happens over a bump.
        return post(core::state::Command(core::state::CommandId::TripReset,
                                         int32_t{0}));
    }
    return false;
}

WidgetList TripScreen::widgets() { return WidgetList{list_, 7}; }

// --- TripMapScreen ------------------------------------------------------
namespace {
constexpr Rect kTMapView { 10,  40, 300, 300};
constexpr Rect kTMapDist { 16, 360, 140,  60};
constexpr Rect kTMapHead {166, 360, 140,  60};
}  // namespace

void TripMapScreen::enter(Canvas&) {
    map_.set_bounds(kTMapView);
    distance_.set_bounds(kTMapDist);
    distance_.configure("TRIP A", "mi", 1, 3);
    heading_.set_bounds(kTMapHead);
    heading_.configure("COURSE", "deg", 0, 3);

    list_[0] = &map_;
    list_[1] = &distance_;
    list_[2] = &heading_;
}

void TripMapScreen::update(const Frame& f) {
    const char* dunit = "mi";
    distance_.set(u::distance_for_display(f.trip.trip_a_mi, units(), &dunit), true);
    distance_.set_unit(dunit);

    // Course over ground, not a compass: the IMU has no magnetometer, and
    // there is no software fix for a part that has no magnetic sensing
    // element in it (§8). Stationary, this is meaningless, so it is only
    // shown as valid when the bike is actually moving.
    const bool moving = f.gnss.fix && f.gnss.speed_mph > 3.0f;
    heading_.set(f.gnss.course_deg, moving);

    map_.set_position(0.0f, 0.0f, f.gnss.course_deg, f.gnss.fix);
}

WidgetList TripMapScreen::widgets() { return WidgetList{list_, 3}; }

}  // namespace ui
