#include "ui/screens.hpp"
#include "core/health/health.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"
#include "ui/theme.hpp"
#include <initializer_list>

namespace ui {

using core::state::Frame;
using core::state::Dev;
namespace u = core::units;

// --- SpeedScreen --------------------------------------------------------
//
// Landscape, 480x320. The two numbers that matter — speed and rpm — get the
// biggest tiles and the top half; everything else is reference material
// around the edges.
//
// The tach bar runs up the left edge and flares out across the top, with a
// marker that rides up it. Its range is 0 to the first shift lamp's rpm, so
// the marker reaches the top at the moment lamp one lights: the shift lights
// say nothing below their own threshold, and this covers exactly that gap
// and then hands over.
namespace {
constexpr Color kFrame = color::kText;

// The two strips the tach marker travels through: the straight run, and the
// flare across the top. Both are drawn by the same TachBar::width_at, so the
// split is only about how much has to be repainted when the marker moves —
// it is not a seam in the shape.
constexpr Rect kTachStem  {0, TachBar::kFlareH, TachBar::kStemW,
                           kScreenH - TachBar::kFlareH};
constexpr Rect kTachFlare {0, 0, TachBar::kFlareW, TachBar::kFlareH};

constexpr Rect kRpmTile   { 31,  28, 169, 43};
constexpr Rect kGearTile  {216,   2,  78, 84};
constexpr Rect kSpeedTile {310,  12, 162, 78};
// scale 10 is 70 px of glyph in an 84 px tile.
constexpr Rect kGearText  {216,  10,  78,  70};
constexpr Rect kShiftStrip{136,   2,  72,  20};

constexpr Rect kLean      { 30,  96, 112, 104};
// Centred in the middle column, under the gear tile. It used to share a left
// edge with the media rows; those have since gone left to where the fault
// column ends, and dragging the ball out from under the gear to follow them
// would be squaring up the wrong pair.
constexpr Rect kBall      {176,  92, 142, 150};
constexpr Rect kFault     { 28, 222, 120,  16};
constexpr Rect kFaultWhat { 28, 242, 120,  10};
constexpr Rect kSats      { 28, 266, 120,  10};
constexpr Rect kHdop      { 28, 286, 120,  10};
// Right-aligned at the screen edge, and only as wide as it needs to be:
// fmt_clock is always five characters, 58 px at scale 2. Widget bounds are
// what gets cleared on a repaint, so a clock reserving the whole right half
// of the row would erase the g-ball's edge every minute.
constexpr Rect kClock     {372, 140, 100,  16};
// Song, album and artist, running to the right edge. Wider than the reference
// put it, because real track names are longer than the ones a layout gets
// mocked up with: at 297 this fitted thirteen characters, which truncated most
// of what actually plays.
//
// The left edge is as far left as it can go rather than lined up with the
// g-ball above: 148 is where the GPS and fault column ends, and these rows are
// the only thing on the screen with an appetite for width. Four pixels of gap
// is deliberate — widget bounds are what a repaint clears, so a row that
// reached 148 would erase the edge of the sats line every time a long title
// scrolled, and nothing would redraw it. 324 px is 25 characters at scale 2;
// anything longer still scrolls.
constexpr Rect kSong      {152, 244, 324, MediaRow::kRowH};
constexpr Rect kAlbum     {152, 264, 324, MediaRow::kRowH};
constexpr Rect kArtist    {152, 284, 324, MediaRow::kRowH};

// The odometer and trip meter are the least urgent things here, so they get
// the strip along the bottom that nothing else wants.
constexpr Rect kOdo       { 28, 306, 210,  10};
constexpr Rect kTrip      {262, 306, 210,  10};

// How long the tile shows a jump before going back to being a g-ball. Long
// enough to read after you have stopped concentrating on landing.
constexpr uint32_t kJumpHoldMs = 5000;

// Appends a literal, returning the new length. The screens build strings out
// of the core/units formatters and pieces like this rather than reaching for
// snprintf, which pulls a float formatter into a binary that has no other
// use for one.
size_t append(char* buf, size_t n, size_t k, const char* s) {
    while (*s != '\0' && k + 1 < n) buf[k++] = *s++;
    buf[k] = '\0';
    return k;
}

}  // namespace

void SpeedScreen::enter(Canvas&) {
    // BigNumber centres in its bounds; ValueBox would reserve a strip for a
    // caption, and inside a framed tile the frame is the caption.
    speed_.set_bounds(kSpeedTile.inset(4));
    speed_.configure("", 0, 8);

    rpm_.set_bounds(kRpmTile.inset(3));
    rpm_.configure("", 0, 5);

    shift_.set_bounds(kShiftStrip);

    // The reference layout puts a gear number here. This bike has no gear
    // sensor, so it is inferred from rpm against road speed, and neutral —
    // which really is sensed — wins over the inference.
    gear_.set_bounds(kGearText);
    gear_.configure("-", color::kDim, 10, Align::Center);

    clock_.set_bounds(kClock);
    clock_.configure("--:--", color::kText, 2, Align::Right);

    bar_stem_.set_bounds(kTachStem);
    bar_stem_.configure(kTachStem.y, kTachStem.bottom());
    bar_flare_.set_bounds(kTachFlare);
    bar_flare_.configure(kTachFlare.y, kTachFlare.bottom());

    lean_.set_bounds(kLean);
    ball_.set_bounds(kBall);
    sats_.set_bounds(kSats);  sats_.configure("", color::kText, 1, Align::Left);
    hdop_.set_bounds(kHdop);  hdop_.configure("", color::kDim, 1, Align::Left);
    fault_.set_bounds(kFault);
    fault_.configure("", theme::alarm, 2, Align::Left);
    fault_what_.set_bounds(kFaultWhat);
    fault_what_.configure("", theme::secondary, 1, Align::Left);
    song_.set_bounds(kSong);
    song_.configure(MediaRow::Kind::Song, color::kText);
    album_.set_bounds(kAlbum);
    album_.configure(MediaRow::Kind::Album, color::kDim);
    artist_.set_bounds(kArtist);
    artist_.configure(MediaRow::Kind::Artist, color::kDim);

    odo_.set_bounds(kOdo);    odo_.configure("", color::kDim, 1, Align::Left);
    trip_.set_bounds(kTrip);  trip_.configure("", color::kDim, 1, Align::Right);

    list_[0]  = &bar_stem_; list_[1]  = &bar_flare_;  list_[2]  = &speed_;
    list_[3]  = &rpm_;      list_[4]  = &shift_;      list_[5]  = &gear_;
    list_[6]  = &clock_;    list_[7]  = &lean_;       list_[8]  = &sats_;
    list_[9]  = &hdop_;     list_[10] = &odo_;        list_[11] = &trip_;
    list_[12] = &ball_;     list_[13] = &fault_;      list_[14] = &fault_what_;
    list_[15] = &song_;     list_[16] = &album_;      list_[17] = &artist_;
}

void SpeedScreen::update(const Frame& f) {
    const char* unit = "mph";
    const float shown = u::speed_for_display(f.motion.best_mph, units(), &unit);
    const bool speed_ok = f.health.ok(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(shown, speed_ok);

    rpm_.set(f.engine.rpm, f.engine.rpm > 0.0f || f.engine.neutral);
    shift_.set(f.engine.shift_mask, f.engine.shift_flash);

    // Neutral is sensed; the gear is inferred. An unknown gear is a dash
    // rather than a stale digit.
    char g[2] = {'-', '\0'};
    Color gc = color::kDim;
    if (f.engine.neutral) {
        g[0] = 'N';
        gc = color::kGood;
    } else if (f.engine.gear > 0 && f.engine.gear <= 9) {
        g[0] = static_cast<char>('0' + f.engine.gear);
        gc = color::kText;
    }
    gear_.configure(g, gc, 10, Align::Center);

    char buf[24];
    u::fmt_clock(buf, sizeof buf, f.gnss.utc_ms,
                 has_ctx() && ctx().settings ? ctx().settings->tz_offset_h : 0,
                 f.gnss.fix);
    clock_.set_text(buf);

    // Full scale is the first shift lamp, not the redline: above that the
    // lamps are the instrument and the bar just stays full.
    const uint16_t base = has_ctx() && ctx().settings
                              ? ctx().settings->shift_base_rpm : 6000;
    const float frac = base > 0 ? f.engine.rpm / static_cast<float>(base) : 0.0f;
    bar_stem_.set(frac);
    bar_flare_.set(frac);

    const bool imu_ok = f.health.ok(Dev::Imu);
    lean_.set(f.att.lean_deg, imu_ok);

    // The g-ball, except for a few seconds after a landing, when the same
    // tile is the jump. `land_ms` is in the Frame precisely so this screen
    // does not have to remember anything between updates.
    ball_.set(f.att.g_lat, f.att.g_lon, imu_ok);
    const bool fresh_jump = f.att.land_ms != 0 &&
                            (f.t_ms - f.att.land_ms) < kJumpHoldMs;
    ball_.set_jump(f.att.airtime_ms, f.att.last_land_g, fresh_jump && imu_ok);

    // The fault line. Blank when everything fitted is healthy, which means
    // the space costs nothing on a normal ride and the screen stops
    // degrading silently: a lean gauge showing `--` now says why.
    const core::health::Worst w = core::health::worst(f.health);
    if (w.any()) {
        fault_.configure(core::state::dev_name(w.dev),
                         w.state == core::state::DevState::Fault ? theme::alarm
                                                                 : theme::warning,
                         2, Align::Left);
        size_t j = append(buf, sizeof buf, 0,
                          w.state == core::state::DevState::Fault ? "FAULT"
                                                                  : "DEGRADED");
        if (w.count > 1) {
            j = append(buf, sizeof buf, j, "  +");
            j += u::fmt_value(buf + j, sizeof buf - j,
                              static_cast<float>(w.count - 1), 0, true);
        }
        fault_what_.set_text(buf);
    } else {
        fault_.set_text("");
        fault_what_.set_text("");
    }

    // What is playing. `updated_ms` of zero is the same gate the diag screen
    // uses: nothing has ever arrived, so there is no media rather than a
    // song with no name. The whole block including its icons disappears.
    const bool playing = f.media.updated_ms != 0;
    song_.set(f.media.title, playing, f.t_ms);
    album_.set(f.media.album, playing, f.t_ms);
    artist_.set(f.media.artist, playing, f.t_ms);

    size_t k = u::fmt_value(buf, sizeof buf, static_cast<float>(f.gnss.sats), 0,
                            f.gnss.fix);
    if (k + 6 < sizeof buf) {
        buf[k++] = ' '; buf[k++] = 'S'; buf[k++] = 'a'; buf[k++] = 't';
        buf[k++] = 's'; buf[k] = '\0';
    }
    sats_.configure(buf, f.gnss.fix ? color::kText : color::kDim, 1, Align::Left);

    k = u::fmt_value(buf, sizeof buf, f.gnss.hdop * 16.0f, 0, f.gnss.fix);
    if (k + 4 < sizeof buf) {
        buf[k++] = ' '; buf[k++] = 'f'; buf[k++] = 't'; buf[k] = '\0';
    }
    hdop_.set_text(buf);

    const char* dunit = "mi";
    k = u::fmt_float(buf, sizeof buf,
                     u::distance_for_display(f.trip.odo_mi, units(), &dunit), 0);
    if (k + 8 < sizeof buf) {
        buf[k++] = ' ';
        for (const char* q = dunit; *q; ++q) buf[k++] = *q;
        buf[k] = '\0';
    }
    odo_.set_text(buf);

    k = u::fmt_float(buf, sizeof buf,
                     u::distance_for_display(f.trip.trip_a_mi, units(), &dunit), 1);
    if (k + 10 < sizeof buf) {
        buf[k++] = ' ';
        for (const char* q = dunit; *q; ++q) buf[k++] = *q;
        buf[k++] = ' '; buf[k++] = 'A'; buf[k] = '\0';
    }
    trip_.set_text(buf);
}

// Only what never changes: the tile frames and the unit labels. Everything
// with data in it is a widget above.
void SpeedScreen::draw_background(Canvas& c) {
    c.draw_round_rect(kRpmTile, 6, kFrame);
    c.draw_round_rect(kGearTile, 8, kFrame);
    c.draw_round_rect(kSpeedTile, 8, kFrame);

    const char* sunit = units() == core::units::System::Metric ? "km/h" : "mph";
    c.draw_text(static_cast<int16_t>(kSpeedTile.right() - 6),
                static_cast<int16_t>(kSpeedTile.bottom() + 4), sunit,
                color::kDim, 1, Align::Right);
    c.draw_text(static_cast<int16_t>(kRpmTile.right() - 6),
                static_cast<int16_t>(kRpmTile.bottom() + 4), "rpm",
                color::kDim, 1, Align::Right);
}

WidgetList SpeedScreen::widgets() { return WidgetList{list_, 18}; }

// --- MinimalScreen ------------------------------------------------------
//
// One number, as large as 480x320 allows, and the clock along the bottom.
// Scale 22 is 154 px of digit — three figures still fit, which matters
// exactly once, going down something steep.
namespace {
constexpr Rect kMinSpeed { 0,  40, 480, 210};
constexpr Rect kMinClock { 0, 288, 480,  18};
}  // namespace

void MinimalScreen::enter(Canvas&) {
    speed_.set_bounds(kMinSpeed);
    speed_.configure("mph", 0, 22);

    clock_.set_bounds(kMinClock);
    clock_.configure("--:--", color::kDim, 2, Align::Center);

    list_[0] = &speed_;
    list_[1] = &clock_;
}

void MinimalScreen::update(const Frame& f) {
    const char* unit = "mph";
    const float shown = u::speed_for_display(f.motion.best_mph, units(), &unit);
    speed_.configure(unit, 0, 22);
    const bool speed_ok = f.health.ok(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(shown, speed_ok);

    char clock[12];
    u::fmt_clock(clock, sizeof clock, f.gnss.utc_ms,
                 has_ctx() && ctx().settings ? ctx().settings->tz_offset_h : 0,
                 f.gnss.fix);
    clock_.set_text(clock);
}

WidgetList MinimalScreen::widgets() { return WidgetList{list_, 2}; }

// --- TripScreen ---------------------------------------------------------
namespace {
// 480x320: the odometer across the top, then two rows of three.
constexpr Rect kTripOdo  { 14,  44, 452, 66};
constexpr Rect kTripA    { 14, 132, 144, 72};
constexpr Rect kTripB    {168, 132, 144, 72};
constexpr Rect kTripFuel {322, 132, 144, 72};
constexpr Rect kTripHours{ 14, 222, 144, 72};
constexpr Rect kTripTop  {168, 222, 144, 72};
constexpr Rect kTripHint {322, 252, 144, 16};
}  // namespace

void TripScreen::enter(Canvas&) {
    odo_.set_bounds(kTripOdo.inset(8));    odo_.configure("ODOMETER", "mi", 1, 5);
    trip_a_.set_bounds(kTripA.inset(7));   trip_a_.configure("TRIP A", "mi", 1, 3);
    trip_b_.set_bounds(kTripB.inset(7));   trip_b_.configure("TRIP B", "mi", 1, 3);
    fuel_.set_bounds(kTripFuel.inset(7));  fuel_.configure("FUEL EST", "gal", 2, 3);
    hours_.set_bounds(kTripHours.inset(7));hours_.configure("ENGINE", "h", 1, 3);
    top_.set_bounds(kTripTop.inset(7));    top_.configure("TOP", "mph", 0, 3);
    hint_.set_bounds(kTripHint);
    hint_.configure("hold E to reset trip A", color::kDim, 1, Align::Left);

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

void TripScreen::draw_background(Canvas& c) {
    for (const Rect& r : {kTripOdo, kTripA, kTripB, kTripFuel, kTripHours,
                          kTripTop}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
    // The fuel figure is a model, not a measurement — there is no flow
    // sensor and no ECU on this bike — so it says so rather than sitting
    // next to five measured numbers pretending to be one of them.
    c.draw_text(kTripFuel.x, static_cast<int16_t>(kTripFuel.bottom() + 4),
                "estimate", theme::secondary, 1, Align::Left);
}

WidgetList TripScreen::widgets() { return WidgetList{list_, 7}; }

// --- TripMapScreen ------------------------------------------------------
namespace {
// Landscape suits a map: it goes on the left, square, with the numbers
// stacked beside it instead of squashed underneath.
constexpr Rect kTMapView { 14,  32, 286, 276};
constexpr Rect kTMapDist {314,  44, 152,  72};
constexpr Rect kTMapHead {314, 138, 152,  72};
constexpr Rect kTMapSpeed{314, 232, 152,  72};
}  // namespace

void TripMapScreen::enter(Canvas&) {
    map_.set_bounds(kTMapView);
    distance_.set_bounds(kTMapDist.inset(7));
    distance_.configure("TRIP A", "mi", 1, 3);
    heading_.set_bounds(kTMapHead.inset(7));
    heading_.configure("COURSE", "deg", 0, 3);
    speed_.set_bounds(kTMapSpeed.inset(7));
    speed_.configure("SPEED", "mph", 0, 3);

    list_[0] = &map_;
    list_[1] = &distance_;
    list_[2] = &heading_;
    list_[3] = &speed_;
}

void TripMapScreen::draw_background(Canvas& c) {
    for (const Rect& r : {kTMapDist, kTMapHead, kTMapSpeed}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
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

    const char* sunit = "mph";
    speed_.set(u::speed_for_display(f.motion.best_mph, units(), &sunit),
               f.gnss.fix);
    speed_.set_unit(sunit);

    map_.set_position(0.0f, 0.0f, f.gnss.course_deg, f.gnss.fix);
}

WidgetList TripMapScreen::widgets() { return WidgetList{list_, 4}; }

}  // namespace ui
