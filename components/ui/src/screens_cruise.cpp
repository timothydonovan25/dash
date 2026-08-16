#include "ui/screens.hpp"
#include "core/units/units.hpp"

namespace ui {

using core::state::Frame;
using core::state::Dev;
namespace u = core::units;

// --- SpeedScreen --------------------------------------------------------
//
// Landscape, 480x320. Laid out the way a rider actually scans it: the two
// numbers that matter — speed and rpm — get the biggest tiles and the top
// half; everything else is reference material along the bottom.
//
// The tach bar is an L down the left edge and across the top, cut on a
// diagonal, which is what gives the panel a direction to be read in. It
// fills with rpm rather than being decoration.
namespace {
constexpr Color kFrame = color::kText;

// Tach bar: the L, and the diagonal that cuts its corner off.
constexpr int16_t kBarTop    = 25;    // thickness of the top run
constexpr int16_t kBarLeft   = 25;    // thickness of the left run
constexpr int16_t kBarTopEnd = 135;   // where the top run stops being full
constexpr int16_t kBarCut    = 234;   // where the diagonal meets the top edge

constexpr Rect kRpmTile   { 31,  28, 169, 43};
constexpr Rect kGearTile  {216,   2,  78, 84};
constexpr Rect kSpeedTile {310,  12, 162, 78};

constexpr int16_t kLeanCx  = 86;
constexpr int16_t kLeanCy  = 155;
constexpr int16_t kLeanR   = 52;
constexpr int16_t kLeanRi  = 40;

constexpr Rect kLeanText  { 46, 140,  80,  30};
constexpr Rect kSats      { 28, 268, 110,  14};
constexpr Rect kHdop      { 28, 288, 110,  14};
constexpr Rect kClock     {297, 140, 175,  20};
constexpr Rect kLapTile   {297, 175, 175,  34};
// scale 10 is 70 px of glyph in an 84 px tile.
constexpr Rect kGearText  {216,  10,  78,  70};
constexpr Rect kTripTile  {216, 250, 250,  20};
constexpr Rect kShiftStrip{136,   2,  72,  20};
}  // namespace

void SpeedScreen::enter(Canvas&) {
    // BigNumber centres in its bounds; ValueBox would reserve a strip for a
    // caption, and inside a framed tile the frame is the caption.
    speed_.set_bounds(kSpeedTile.inset(4));
    speed_.configure("", 0, 8);

    rpm_.set_bounds(kRpmTile.inset(3));
    rpm_.configure("", 0, 5);

    shift_.set_bounds(kShiftStrip);

    // The reference puts a gear number here. This bike has no gear sensor,
    // and a gear guessed from rpm over speed is wrong on every clutch pull,
    // so the tile shows the one thing the bike does report: neutral.
    gear_.set_bounds(kGearText);
    gear_.configure("-", color::kDim, 10, Align::Center);

    clock_.set_bounds(kClock);
    clock_.configure("--:--", color::kText, 2, Align::Right);

    list_[0] = &speed_;
    list_[1] = &rpm_;
    list_[2] = &shift_;
    list_[3] = &gear_;
    list_[4] = &clock_;
}

void SpeedScreen::update(const Frame& f) {
    const char* unit = "mph";
    const float shown = u::speed_for_display(f.motion.best_mph, units(), &unit);
    const bool speed_ok = f.health.ok(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(shown, speed_ok);

    rpm_.set(f.engine.rpm, f.engine.rpm > 0.0f || f.engine.neutral);
    shift_.set(f.engine.shift_mask, f.engine.shift_flash);

    gear_.configure(f.engine.neutral ? "N" : "-",
                    f.engine.neutral ? color::kGood : color::kDim, 10,
                    Align::Center);

    char clock[12];
    u::fmt_clock(clock, sizeof clock, f.gnss.utc_ms,
                 has_ctx() && ctx().settings ? ctx().settings->tz_offset_h : 0,
                 f.gnss.fix);
    clock_.set_text(clock);

    lean_deg_ = f.att.lean_deg;
    lean_ok_ = f.health.ok(Dev::Imu);
    sats_ = f.gnss.sats;
    hdop_ft_ = f.gnss.hdop * 16.0f;      // rough DOP-to-feet, for a glance
    fix_ = f.gnss.fix;
    rpm_frac_ = f.engine.rpm / 11000.0f;
    if (rpm_frac_ < 0.0f) rpm_frac_ = 0.0f;
    if (rpm_frac_ > 1.0f) rpm_frac_ = 1.0f;
    lap_ms_ = f.lap.current_ms;
    lap_timing_ = f.lap.phase == core::lap::Phase::Timing;
    trip_mi_ = f.trip.trip_a_mi;
    odo_mi_ = f.trip.odo_mi;
    chrome_dirty_ = true;
}

// The parts of the screen that are not a widget: the bar, the tiles, the
// lean gauge and the small print. Drawn by the screen itself because they
// are the screen's identity rather than reusable furniture.
void SpeedScreen::draw_chrome(Canvas& c) {
    // The L, with its corner cut away on the diagonal.
    c.fill_rect(Rect{0, kBarTop, kBarLeft,
                     static_cast<int16_t>(kScreenH - kBarTop)}, color::kTach);
    c.fill_rect(Rect{0, 0, kBarTopEnd, kBarTop}, color::kTach);
    c.fill_triangle(kBarTopEnd, 0, kBarCut, kBarTop, 27, kBarTop, color::kBg);

    // The left run fills from the bottom with rpm, so the bar is an
    // instrument rather than a border.
    const int16_t lit = static_cast<int16_t>((kScreenH - kBarTop) * rpm_frac_);
    if (lit > 0) {
        c.fill_rect(Rect{0, static_cast<int16_t>(kScreenH - lit),
                         kBarLeft, lit}, color::kWhite);
    }

    c.draw_round_rect(kRpmTile, 6, kFrame);
    c.draw_round_rect(kGearTile, 8, kFrame);
    c.draw_round_rect(kSpeedTile, 8, kFrame);

    // Units live on the frame, not in the number, so the digits get the room.
    const char* sunit = units() == core::units::System::Metric ? "km/h" : "mph";
    c.draw_text(static_cast<int16_t>(kSpeedTile.right() - 6),
                static_cast<int16_t>(kSpeedTile.bottom() + 4), sunit,
                color::kDim, 1, Align::Right);
    c.draw_text(static_cast<int16_t>(kRpmTile.right() - 6),
                static_cast<int16_t>(kRpmTile.bottom() + 4), "rpm",
                color::kDim, 1, Align::Right);

    // --- lean gauge -----------------------------------------------------
    // A half-annulus from nine o'clock round the top to three o'clock, with
    // the rider's angle marked on it. Peak marks are deliberately absent:
    // the number below is the reading, the arc is the feel.
    c.fill_arc(kLeanCx, kLeanCy, kLeanR, kLeanRi, 270.0f, 360.0f, color::kGrid);
    c.fill_arc(kLeanCx, kLeanCy, kLeanR, kLeanRi, 0.0f, 90.0f, color::kGrid);

    if (lean_ok_) {
        float a = lean_deg_;
        if (a < -85.0f) a = -85.0f;
        if (a > 85.0f) a = 85.0f;
        // Lean right tips the marker right, which is the direction the rider
        // is going. Zero is straight up.
        const float mark = a < 0.0f ? 360.0f + a : a;
        c.fill_arc(kLeanCx, kLeanCy, kLeanR, kLeanRi,
                   mark - 4.0f, mark + 4.0f, color::kTach);
    }

    char buf[16];
    u::fmt_value(buf, sizeof buf, lean_deg_ < 0 ? -lean_deg_ : lean_deg_, 0,
                 lean_ok_);
    c.draw_text(kLeanText.cx(), kLeanText.y, buf,
                lean_ok_ ? color::kText : color::kDim, 3, Align::Center);

    // --- GPS quality ----------------------------------------------------
    size_t k = u::fmt_value(buf, sizeof buf, static_cast<float>(sats_), 0, fix_);
    if (k + 6 < sizeof buf) {
        buf[k++] = ' '; buf[k++] = 'S'; buf[k++] = 'a'; buf[k++] = 't';
        buf[k++] = 's'; buf[k] = '\0';
    }
    c.draw_text(kSats.x, kSats.y, buf, fix_ ? color::kText : color::kDim, 1,
                Align::Left);

    k = u::fmt_value(buf, sizeof buf, hdop_ft_, 0, fix_);
    if (k + 4 < sizeof buf) {
        buf[k++] = ' '; buf[k++] = 'f'; buf[k++] = 't'; buf[k] = '\0';
    }
    c.draw_text(kHdop.x, kHdop.y, buf, color::kDim, 1, Align::Left);

    // --- lap clock, where the media strip sits in the reference ---------
    u::fmt_duration(buf, sizeof buf, lap_ms_, lap_timing_);
    c.draw_text(kLapTile.right(), kLapTile.y, buf,
                lap_timing_ ? color::kAccent : color::kDim, 2, Align::Right);
    c.draw_text(kLapTile.right(), static_cast<int16_t>(kLapTile.y - 12), "LAP",
                color::kDim, 1, Align::Right);

    // --- odometer and trip ----------------------------------------------
    const char* dunit = "mi";
    char line[32];
    k = u::fmt_float(line, sizeof line,
                     u::distance_for_display(odo_mi_, units(), &dunit), 0);
    if (k + 8 < sizeof line) {
        line[k++] = ' ';
        for (const char* q = dunit; *q; ++q) line[k++] = *q;
        line[k] = '\0';
    }
    c.draw_text(kTripTile.x, kTripTile.y, line, color::kDim, 1, Align::Left);

    k = u::fmt_float(line, sizeof line,
                     u::distance_for_display(trip_mi_, units(), &dunit), 1);
    if (k + 10 < sizeof line) {
        line[k++] = ' ';
        for (const char* q = dunit; *q; ++q) line[k++] = *q;
        line[k++] = ' '; line[k++] = 'A'; line[k] = '\0';
    }
    c.draw_text(kTripTile.right(), kTripTile.y, line, color::kDim, 1,
                Align::Right);
}

WidgetList SpeedScreen::widgets() { return WidgetList{list_, 5}; }

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
