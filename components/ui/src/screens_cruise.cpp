#include "ui/screens.hpp"
#include "core/stunt/stunt.hpp"
#include "core/geo/geo.hpp"
#include "core/health/health.hpp"
#include "core/route/navigator.hpp"
#include "core/route/search.hpp"
#include "core/units/units.hpp"
#include "core/waypoint/waypoint.hpp"
#include "ui/chrome.hpp"
#include "ui/theme.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
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
// The rider's heart rate, under the rpm tile and left of the lean gauge
// (which starts at x=116): the bpm at scale 4, its unit small under it as
// rpm and mph have theirs. Blank unless CONFIG -> Heart rate -> Watch HR.
constexpr Rect kHr        { 28, 100,  84,  28};
constexpr Rect kHrUnit    { 28, 132,  84,  10};
// The posted limit of the road the route is on, under the heart rate the
// same way: the number at scale 4, LIMIT under it. Blank unless a route is
// being followed and the road has a tagged limit.
constexpr Rect kLim       { 28, 152,  84,  28};
constexpr Rect kLimCap    { 28, 184,  84,  10};

// The middle band is the lean gauge alone, as big as the band allows: this
// is read on a moving dirt bike, where a g-ball's dot was detail nobody
// could take in. Centred across the screen, between the top tiles (which
// end at y=90) and the fault line and media ring (from y=222). The clock
// sits at the right edge, level with the number. Around a jump the gauge
// shows the jump instead (LeanGauge's other face, kJumpShowMs below).
constexpr Rect kLean      {116,  94, 248, 126};
// How long the lean gauge shows the last jump after landing (LeanGauge's
// landed face) before it goes back to lean.
constexpr uint32_t kJumpShowMs = 4000;
// And a scored wheelie or stoppie, as long as the toast holds one elsewhere.
constexpr uint32_t kTrickShowMs = 3500;
constexpr Rect kFault     { 28, 222, 120,  16};
constexpr Rect kFaultWhat { 28, 242, 120,  10};
constexpr Rect kSats      { 28, 266, 120,  10};
constexpr Rect kHdop      { 28, 286, 120,  10};
// Right-aligned at the screen edge, and only as wide as it needs to be:
// fmt_clock is always five characters, 58 px at scale 2. Widget bounds are
// what gets cleared on a repaint, so a clock reserving the whole right half
// of the row would erase the lean gauge's edge every minute.
constexpr Rect kClock     {410, 186,  62,  16};
// The media block along the bottom: the progress ring, then song, album and
// artist beside it. The ring shares the rows' bottom edge and rises to
// the lean gauge's (y=220), 24 px above the rows. It starts where the
// GPS and fault column ends, plus 4 px — widget bounds are what a repaint
// clears, so anything reaching 148 would erase the edge of the sats line
// and nothing would redraw it. The rows take the rest of the width to the
// right edge: 232 px is 19 characters at scale 2, and longer still scrolls.
constexpr Rect kMediaArc  {152, 220,  84,  84};
constexpr Rect kSong      {244, 244, 232, MediaRow::kRowH};
constexpr Rect kAlbum     {244, 264, 232, MediaRow::kRowH};
constexpr Rect kArtist    {244, 284, 232, MediaRow::kRowH};

// The odometer and trip meter are the least urgent things here, so they get
// the strip along the bottom that nothing else wants.
constexpr Rect kOdo       { 28, 306, 210,  10};
constexpr Rect kTrip      {262, 306, 210,  10};


// Appends a literal, returning the new length. The screens build strings out
// of the core/units formatters and pieces like this rather than reaching for
// snprintf, which pulls a float formatter into a binary that has no other
// use for one.
size_t append(char* buf, size_t n, size_t k, const char* s) {
    while (*s != '\0' && k + 1 < n) buf[k++] = *s++;
    buf[k] = '\0';
    return k;
}

// §11.1's `P` tag is an anchor, not a per-tick position — the phone does
// not write a fresh one every second of playback, so the live elapsed
// time is that anchor plus however long this screen's own clock has moved
// since, and only while state says it is actually playing (state 1).
// Paused or stopped, the anchor already is the answer. Shared by every
// screen with a media block, so SpeedScreen and MediaScreen can never
// quietly disagree about what "now" means for a paused track.
uint32_t media_elapsed_ms(const core::state::Media& m, uint32_t t_ms) {
    uint32_t elapsed = m.position_ms;
    if (m.state == 1 && t_ms >= m.position_at_ms) {
        elapsed += t_ms - m.position_at_ms;
    }
    if (m.duration_ms > 0 && elapsed > m.duration_ms) {
        elapsed = m.duration_ms;
    }
    return elapsed;
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

    hr_.set_bounds(kHr);
    hr_.configure("", color::kDim, 4, Align::Center);
    hr_unit_.set_bounds(kHrUnit);
    hr_unit_.configure("", theme::secondary, 1, Align::Center);
    lim_.set_bounds(kLim);
    lim_.configure("", color::kText, 4, Align::Center);
    lim_cap_.set_bounds(kLimCap);
    lim_cap_.configure("", theme::secondary, 1, Align::Center);

    bar_stem_.set_bounds(kTachStem);
    bar_stem_.configure(kTachStem.y, kTachStem.bottom());
    bar_flare_.set_bounds(kTachFlare);
    bar_flare_.configure(kTachFlare.y, kTachFlare.bottom());

    lean_.set_bounds(kLean);
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
    progress_.set_bounds(kMediaArc);

    odo_.set_bounds(kOdo);    odo_.configure("", color::kDim, 1, Align::Left);
    trip_.set_bounds(kTrip);  trip_.configure("", color::kDim, 1, Align::Right);

    list_[0]  = &bar_stem_; list_[1]  = &bar_flare_;  list_[2]  = &speed_;
    list_[3]  = &rpm_;      list_[4]  = &shift_;      list_[5]  = &gear_;
    list_[6]  = &clock_;    list_[7]  = &lean_;       list_[8]  = &sats_;
    list_[9]  = &hdop_;     list_[10] = &odo_;        list_[11] = &trip_;
    list_[12] = &fault_;    list_[13] = &fault_what_; list_[14] = &song_;
    list_[15] = &album_;    list_[16] = &artist_;     list_[17] = &progress_;
    list_[18] = &hr_;       list_[19] = &hr_unit_;    list_[20] = &lim_;
    list_[21] = &lim_cap_;
}

void SpeedScreen::update(const Frame& f) {
    const char* unit = "mph";
    // CONFIG -> Display -> Speedo. Fused only while the filter vouches for
    // itself; otherwise the wheels, as always.
    const bool fused = has_ctx() && ctx().settings && ctx().settings->speedo_source == 1 &&
                       f.fused.speed_valid;
    const float mph = fused ? f.fused.speed_mph : f.motion.best_mph;
    const float shown = u::speed_for_display(mph, units(), &unit);
    const bool speed_ok = f.health.working(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(shown, speed_ok);

    rpm_.set(u::rpm_for_display(f.engine.rpm), f.engine.rpm > 0.0f || f.engine.neutral);
    // The held lamp too, as the handlebar shows it (Engine::shift_hold).
    shift_.set(static_cast<uint8_t>(f.engine.shift_mask | f.engine.shift_hold),
               f.engine.shift_flash);

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
                 f.gnss.fix, has_ctx() && ctx().settings && ctx().settings->clock_12h);
    clock_.set_text(buf);

    const Color hc = chrome::hr_readout(f.heart, f.t_ms, buf, sizeof buf, "");
    hr_.configure(buf, hc, 4, Align::Center);
    hr_unit_.set_text(buf[0] ? "bpm" : "");

    const bool lim = f.nav.following && f.nav.limit_mph;
    buf[0] = '\0';
    if (lim) std::snprintf(buf, sizeof buf, "%u", unsigned(f.nav.limit_mph));
    lim_.set_text(buf);
    lim_cap_.set_text(lim ? "LIMIT" : "");

    // Full scale is the first shift lamp, not the redline: above that the
    // lamps are the instrument and the bar just stays full.
    const uint16_t base = has_ctx() && ctx().settings
                              ? ctx().settings->shift_base_rpm : 6000;
    const float frac = base > 0 ? f.engine.rpm / static_cast<float>(base) : 0.0f;
    bar_stem_.set(frac);
    bar_flare_.set(frac);
    // Where it got to before the shift, held a moment (core::shift::PeakHold).
    const float peak = base > 0 && f.engine.rpm_peak > 0.0f
                           ? f.engine.rpm_peak / static_cast<float>(base) : 0.0f;
    bar_stem_.set_peak(peak);
    bar_flare_.set_peak(peak);
    bar_stem_.set_scale(base);
    bar_flare_.set_scale(base);

    const bool imu_ok = f.health.ok(Dev::Imu);
    lean_.set(f.att.lean_deg, imu_ok);
    // The jump takes the gauge's place: counting in the air, then the
    // result for a few seconds after the landing. core/jump owns the
    // detection; `land_ms` is what lets a Frame, which has no history, know
    // how long ago that was. A dead IMU shows lean as `--`, not a jump.
    //
    // Wheelies and stoppies (core::stunt) take it too, here rather than as
    // the toast every other screen shows (ui::App): the trick under way,
    // then the score for a few seconds. In the air is always the jump; after
    // that, whichever of a landing and a scored trick is newer. CONFIG ->
    // Display -> Popups turns the trick faces off with the toast.
    using Face = LeanGauge::Face;
    namespace st = core::stunt;
    const core::state::Stunt& s = f.stunt;
    const bool tricks = imu_ok && (!has_ctx() || !ctx().settings || ctx().settings->popups);
    const bool landed = imu_ok && f.att.land_ms != 0 && static_cast<int32_t>(f.t_ms - f.att.land_ms) < static_cast<int32_t>(kJumpShowMs);
    const bool scored = tricks && s.seq != 0 && s.done_ms != 0 && static_cast<int32_t>(f.t_ms - s.done_ms) < static_cast<int32_t>(kTrickShowMs);
    Face face = Face::Lean;
    if (imu_ok && f.att.airborne) {
        face = Face::Air;
    } else if (tricks && s.live_kind != st::None) {
        face = Face::Trick;
    } else if (landed && scored) {
        face = f.att.land_ms - s.done_ms < 0x80000000u ? Face::Landed : Face::TrickDone;
    } else if (landed) {
        face = Face::Landed;
    } else if (scored) {
        face = Face::TrickDone;
    }

    if (face == Face::Trick) {
        char big[12], detail[24], t[12];
        const size_t k = u::fmt_float(t, sizeof t, s.live_ms / 1000.0f, 1);
        std::snprintf(big, sizeof big, "%.*ss", static_cast<int>(k), t);
        const float deg = s.live_kind == st::Stoppie ? -s.live_deg : s.live_deg;
        std::snprintf(detail, sizeof detail, "%d deg  +%u", static_cast<int>(deg + 0.5f),
                      static_cast<unsigned>(s.live_points * s.combo_x10 / 10.0f));
        lean_.set_trick(face, s.live_kind == st::Stoppie ? "STOPPIE" : "WHEELIE",
                        s.live_balance ? color::kWarn : color::kAccent, big, detail);
    } else if (face == Face::TrickDone) {
        char big[12], detail[24], t[12];
        std::snprintf(big, sizeof big, "+%u", static_cast<unsigned>(s.points));
        const size_t k = u::fmt_float(t, sizeof t, s.ms / 1000.0f, 1);
        if (s.kind == st::Looped) {
            std::snprintf(detail, sizeof detail, "combo lost");
        } else if (s.mult_x10 > 10) {
            std::snprintf(detail, sizeof detail, "%.*ss %d deg x%u.%u", static_cast<int>(k), t,
                          static_cast<int>(s.peak_deg + 0.5f), s.mult_x10 / 10u,
                          s.mult_x10 % 10u);
        } else {
            const char* land = st::landing_name(s.landing);
            std::snprintf(detail, sizeof detail, "%.*ss %d deg %s", static_cast<int>(k), t,
                          static_cast<int>(s.peak_deg + 0.5f), land);
        }
        const Color c = s.kind == st::Looped ? color::kAlarm
                      : s.record ? color::kWarn : color::kGood;
        lean_.set_trick(face, st::grade_name(s.kind, s.grade), c, big, detail);
    } else {
        lean_.set_jump(face, f.att.airtime_ms, f.att.last_land_g);
    }

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

    progress_.set(media_elapsed_ms(f.media, f.t_ms), f.media.duration_ms,
                 f.media.state, playing);

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

bool SpeedScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    // Long E stays the overlay menu toggle (core::nav's own job); only the
    // three inputs nav does nothing with here are claimed.
    if (e.press != Press::Short) return false;
    int32_t cmd;
    switch (e.button) {
        case Button::C: cmd = 0; break;   // previous
        case Button::D: cmd = 2; break;   // next
        case Button::E: cmd = 1; break;   // play/pause
        default: return false;
    }
    return post(core::state::Command(core::state::CommandId::MediaCtrl, cmd));
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

WidgetList SpeedScreen::widgets() { return WidgetList{list_, 22}; }

const char* SpeedScreen::widget_name(size_t i) const {
    // Same order as list_ above.
    static const char* kNames[22] = {
        "tach stem", "tach flare", "speed", "rpm", "shift", "gear",
        "clock", "lean", "sats", "hdop", "odo", "trip",
        "fault", "fault what", "song", "album", "artist", "progress",
        "hr", "hr unit", "limit", "limit cap",
    };
    return i < 22 ? kNames[i] : nullptr;
}

// --- MediaScreen ----------------------------------------------------------
//
// Everything SpeedScreen's own media block already computes (the play
// state gate, the elapsed-time extrapolation), just laid out for reading
// rather than glancing — see media_elapsed_ms() above, shared by both.
namespace {
constexpr Rect kMediaSpeed  { 20,  40, 130,  90};
// Between the speed tile and the ring, filling what would otherwise be the
// one patch of this screen with nothing in it. Only ever holds one of the
// handful of albums baked into `ui/album_art.hpp` — everything else in a
// library leaves this blank.
constexpr Rect kMediaArt    {170,  30, 120, 120};
constexpr Rect kMediaRing   {300,  30, 150, 150};
constexpr Rect kMediaSong   { 20, 188, 440,  28};
constexpr Rect kMediaAlbum  { 20, 222, 440,  28};
constexpr Rect kMediaArtist { 20, 256, 440,  28};
// The line streamed live from the phone's .lrc reader (§11.1 tag `Y`) — the
// one row here that changes on its own clock, not on a track change.
// Two lines at scale 2, so a 59-character line fits whole without scrolling.
constexpr Rect kMediaLyric  { 20, 286, 440,  33};
}  // namespace

void MediaScreen::enter(Canvas&) {
    speed_.set_bounds(kMediaSpeed.inset(4));
    speed_.configure("", 0, 5);

    art_.set_bounds(kMediaArt);

    progress_.set_bounds(kMediaRing);

    song_.set_bounds(kMediaSong);
    song_.configure(MediaRow::Kind::Song, color::kText, 3);
    album_.set_bounds(kMediaAlbum);
    album_.configure(MediaRow::Kind::Album, color::kDim, 3);
    artist_.set_bounds(kMediaArtist);
    artist_.configure(MediaRow::Kind::Artist, color::kDim, 3);
    lyric_.set_bounds(kMediaLyric);
    // The elapsed time's size, 34 characters a line, wrapped onto a second
    // line rather than scrolled.
    lyric_.configure(MediaRow::Kind::Lyric, color::kAccent, 2, true);

    list_[0] = &speed_;  list_[1] = &art_;    list_[2] = &song_;
    list_[3] = &album_;  list_[4] = &artist_; list_[5] = &lyric_;
    list_[6] = &progress_;
}

void MediaScreen::update(const Frame& f) {
    const bool speed_ok = f.health.working(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(u::speed_for_display(f.motion.best_mph, units(), nullptr), speed_ok);

    const bool playing = f.media.updated_ms != 0;
    art_.set(f.media.album, playing);
    song_.set(f.media.title, playing, f.t_ms);
    album_.set(f.media.album, playing, f.t_ms);
    artist_.set(f.media.artist, playing, f.t_ms);
    lyric_.set(f.media.lyric, playing, f.t_ms);
    progress_.set(media_elapsed_ms(f.media, f.t_ms), f.media.duration_ms,
                 f.media.state, playing);
}

bool MediaScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    if (e.press != Press::Short) return false;
    int32_t cmd;
    switch (e.button) {
        case Button::C: cmd = 0; break;   // previous
        case Button::D: cmd = 2; break;   // next
        case Button::E: cmd = 1; break;   // play/pause
        default: return false;
    }
    return post(core::state::Command(core::state::CommandId::MediaCtrl, cmd));
}

void MediaScreen::draw_background(Canvas& c) {
    c.draw_round_rect(kMediaSpeed, chrome::kRadius, kFrame);
    const char* sunit = units() == core::units::System::Metric ? "km/h" : "mph";
    c.draw_text(static_cast<int16_t>(kMediaSpeed.right() - 6),
                static_cast<int16_t>(kMediaSpeed.bottom() + 4), sunit,
                color::kDim, 1, Align::Right);
}

WidgetList MediaScreen::widgets() { return WidgetList{list_, 7}; }

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
    const bool speed_ok = f.health.working(Dev::Gnss) || f.motion.front_mph > 0.0f ||
                          f.motion.rear_mph > 0.0f;
    speed_.set(shown, speed_ok);

    char clock[12];
    u::fmt_clock(clock, sizeof clock, f.gnss.utc_ms,
                 has_ctx() && ctx().settings ? ctx().settings->tz_offset_h : 0,
                 f.gnss.fix, has_ctx() && ctx().settings && ctx().settings->clock_12h);
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

// --- NAV ------------------------------------------------------------------

// Each view tiles the whole body below the title rule (y 30..320) with no
// gap and no overlap, every widget painting its own background: switching
// views repaints every pixel the other view drew. Text sits at the top of
// its tile.
constexpr Rect kNavGutL   {  0,  30,  14, 290};
constexpr Rect kNavGutR   {466,  30,  14, 290};
// The picker.
constexpr Rect kNavHead   { 14,  30, 452,  24};
constexpr Rect kNavTable  { 14,  54, 452, 232};
constexpr Rect kNavPHint  { 14, 286, 452,  34};
// "Where to?"
constexpr Rect kSrQuery   { 14,  30, 452,  24};
constexpr Rect kSrStatus  { 14,  54, 452,  14};
constexpr Rect kSrKeys    { 14,  68, 452,  90};
constexpr Rect kSrTable   { 14, 158, 452, 128};
// Guidance.
constexpr Rect kNavArrow  { 14,  30, 200, 214};
constexpr Rect kNavGutM   {214,  30,  16, 214};
constexpr Rect kNavDist   {230,  30, 236,  66};
constexpr Rect kNavUnit   {230,  96, 236,  26};
constexpr Rect kNavRoad   {230, 122, 236,  34};
constexpr Rect kNavLimCap {230, 156, 236,  16};
constexpr Rect kNavLimit  {230, 172, 236,  72};
constexpr Rect kNavMap    { 14,  30, 452, 214};   // the arrow, the gutter and the numbers
constexpr Rect kNavRemain { 14, 244, 452,  24};
constexpr Rect kNavStatus { 14, 268, 452,  24};
constexpr Rect kNavHint   { 14, 292, 452,  28};
constexpr Rect kNowhere   {  0,   0,   0,   0};

namespace {
// Feet (or metres) up close, where the turn is in sight; miles (or km)
// further out.
void nav_distance(char* num, size_t n, const char** unit, double dist_ft, u::System sys) {
    const bool metric = sys == u::System::Metric;
    const float mi = static_cast<float>(dist_ft / 5280.0);
    if (mi < 0.1f) {
        const float near = metric ? static_cast<float>(dist_ft * 0.3048) : static_cast<float>(dist_ft);
        // To the nearest 50 ft (or 10 m): the number a rider can use.
        const float step = metric ? 10.0f : 50.0f;
        u::fmt_value(num, n, std::round(near / step) * step, 0, true);
        *unit = metric ? "m" : "ft";
    } else {
        const float far = u::distance_for_display(mi, sys, unit);
        u::fmt_value(num, n, far, far < 10.0f ? 1 : 0, true);
    }
}

// The pace notes as a rally co-driver reads them: the distance to the
// first, each note, and the gap to the next unless it runs "into" it.
// "300 R4 tightens 150 L3 into R5". Numbers without units, as on a pace
// note card; a gap under 50 ft (10 m) is left out.
void pace_line(char* out, size_t cap, const core::state::Nav& n, u::System sys) {
    size_t w = 0;
    out[0] = '\0';
    const auto put = [&](const char* t) {
        const int k = std::snprintf(out + w, cap - w, "%s%s", w ? " " : "", t);
        if (k > 0) w = w + size_t(k) < cap ? w + size_t(k) : cap - 1;
    };
    for (uint8_t i = 0; i < n.n_pace && i < core::state::Nav::kMaxPace; ++i) {
        if (!n.pace_into[i] && n.pace_gap_ft[i] >= 50.0f) {
            char num[12];
            const char* unit = "";
            nav_distance(num, sizeof num, &unit, n.pace_gap_ft[i], sys);
            put(num);
        }
        put(n.pace[i]);
    }
}

constexpr uint16_t kAvoidDirtKey = core::config::kKeyNavAvoidDirt;
constexpr uint16_t kSpeedLimitKey = core::config::kKeyNavSpeedLimit;

bool nav_guiding(uint8_t phase) {
    using core::route::Phase;
    switch (static_cast<Phase>(phase)) {
        case Phase::Routing: case Phase::NoSlowRoute:
        case Phase::Guiding: case Phase::Rerouting: case Phase::Arrived:
            return true;
        default:
            return false;
    }
}

}  // namespace

void NavScreen::enter(Canvas&) {
    gut_l_.set_bounds(kNavGutL);
    gut_r_.set_bounds(kNavGutR);
    head_.configure("", color::kDim, 1, Align::Left);
    table_.configure(11, 2);
    phint_.configure("", theme::secondary, 1, Align::Left);
    squery_.configure("", color::kText, 2, Align::Left);
    sstatus_.configure("", color::kDim, 1, Align::Left);
    dist_.configure("--", color::kText, 6, Align::Left);
    dist_unit_.configure("", color::kDim, 2, Align::Left);
    road_.configure("", color::kText, 2, Align::Left);
    limit_cap_.configure("", theme::secondary, 1, Align::Left);
    limit_.configure("", color::kText, 3, Align::Left);
    remain_.configure("", color::kText, 2, Align::Left);
    status_.configure("", theme::warning, 2, Align::Left);
    hint_.configure("", theme::secondary, 1, Align::Left);
    lay_out(View::Picker);
}

void NavScreen::lay_out(View v) {
    view_ = v;
    laid_out_ = true;
    const bool pick = v == View::Picker, search = v == View::Search, guide = v == View::Guide;
    gut_l_.touch();
    gut_r_.touch();
    head_.set_bounds(pick ? kNavHead : kNowhere);
    table_.set_bounds(pick ? kNavTable : search ? kSrTable : kNowhere);
    table_.configure(pick ? 11 : 6, 2);
    table_.clear_rows();
    phint_.set_bounds(pick || search ? kNavPHint : kNowhere);
    squery_.set_bounds(search ? kSrQuery : kNowhere);
    sstatus_.set_bounds(search ? kSrStatus : kNowhere);
    keys_.set_bounds(search ? kSrKeys : kNowhere);
    const bool turns = guide && !show_map_;
    map_.set_bounds(guide && show_map_ ? kNavMap : kNowhere);
    map_.set_route_only(true);
    gut_m_.set_bounds(turns ? kNavGutM : kNowhere);
    arrow_.set_bounds(turns ? kNavArrow : kNowhere);
    dist_.set_bounds(turns ? kNavDist : kNowhere);
    dist_unit_.set_bounds(turns ? kNavUnit : kNowhere);
    road_.set_bounds(turns ? kNavRoad : kNowhere);
    limit_cap_.set_bounds(turns ? kNavLimCap : kNowhere);
    limit_.set_bounds(turns ? kNavLimit : kNowhere);
    remain_.set_bounds(guide ? kNavRemain : kNowhere);
    status_.set_bounds(guide ? kNavStatus : kNowhere);
    hint_.set_bounds(guide ? kNavHint : kNowhere);
    n_list_ = 0;
    list_[n_list_++] = &gut_l_;
    list_[n_list_++] = &gut_r_;
    if (guide && show_map_) {
        Widget* const g[] = {&map_, &remain_, &status_, &hint_};
        for (Widget* w : g) list_[n_list_++] = w;
    } else if (guide) {
        Widget* const g[] = {&gut_m_, &arrow_, &dist_, &dist_unit_, &road_, &limit_cap_, &limit_,
                             &remain_, &status_, &hint_};
        for (Widget* w : g) list_[n_list_++] = w;
    } else if (search) {
        Widget* const g[] = {&squery_, &sstatus_, &keys_, &table_, &phint_};
        for (Widget* w : g) list_[n_list_++] = w;
    } else {
        list_[n_list_++] = &head_;
        list_[n_list_++] = &table_;
        list_[n_list_++] = &phint_;
    }
    focus_ = 0;
    in_list_ = false;
    reset_focus_ = 0;
    cursor_ = -1;
}

uint8_t NavScreen::focus_count() const {
    switch (view_) {
        case View::Picker: return rows_;
        // The keys are the screen's own (C/D move cursor_); the results are nav's.
        case View::Search: return in_list_ ? n_hits_ : 0;
        default:           return 0;
    }
}

// After the keys on offer change, the cursor stays on its key if it can
// still be pressed, else goes to the nearest one that can (forward first
// on a tie): typing in the digits keeps it in the digits. With no key yet,
// the first letter or digit.
void NavScreen::place_cursor() {
    const auto on = [&](int k) { return k >= 0 && k < KeyGrid::kKeys && ((enabled_ >> k) & 1); };
    if (on(cursor_)) return;
    if (cursor_ < 0) {
        for (int k = 2; k < KeyGrid::kKeys; ++k) if (on(k)) { cursor_ = k; return; }
        cursor_ = on(1) ? 1 : on(0) ? 0 : -1;
        return;
    }
    for (int d = 1; d < KeyGrid::kKeys; ++d) {
        if (on(cursor_ + d)) { cursor_ += d; return; }
        if (on(cursor_ - d)) { cursor_ -= d; return; }
    }
    cursor_ = -1;
}

void NavScreen::update(const Frame& f) {
    const core::state::Nav& n = f.nav;
    phase_ = n.phase;
    avoid_dirt_ = n.avoid_dirt;
    limit_ix_ = 0;
    for (uint8_t i = 0; i < core::config::kNavLimitCount; ++i)
        if (core::config::kNavLimitMph[i] == n.max_mph) limit_ix_ = i;
    const View v = nav_guiding(n.phase) ? View::Guide
                 : n.search != core::state::Nav::SearchOff ? View::Search : View::Picker;
    // No route under the limit shows the map; otherwise the rider's choice.
    using core::route::Phase;
    const bool asking = n.phase == uint8_t(Phase::NoSlowRoute);
    const bool map = v == View::Guide && (asking || want_map_);
    if (v != view_ || map != show_map_) {
        show_map_ = map;
        lay_out(v);
    } else if (n.search != search_) {
        // Spelling to the house number, or back: the cursor to the first key.
        focus_ = 0;
        in_list_ = false;
        reset_focus_ = 0;
        cursor_ = -1;
    }
    search_ = n.search;
    switch (view_) {
        case View::Picker: update_picker(f); break;
        case View::Search: update_search(f); break;
        case View::Guide:  update_guide(f); break;
    }
    if (show_map_) {
        map_.set_labels(n.labels, n.n_labels);
        map_.set_live(f.gnss.fix);
    }
}

void NavScreen::update_picker(const Frame& f) {
    using core::route::Phase;
    const core::state::Nav& n = f.nav;
    const Phase ph = static_cast<Phase>(n.phase);
    char head[64];
    if (ph == Phase::NoMap) {
        std::snprintf(head, sizeof head, "NO MAP: copy route/map.rgf to the card");
    } else if (ph == Phase::Failed) {
        std::snprintf(head, sizeof head, "NO ROUTE: %s",
                      core::route::to_string(static_cast<core::route::Status>(n.status)));
    } else {
        std::snprintf(head, sizeof head, "MAP %04u-%02u-%02u%s", unsigned(n.map_date / 10000),
                      unsigned(n.map_date / 100 % 100), unsigned(n.map_date % 100),
                      f.gnss.fix ? "" : "   waiting for a GPS fix");
    }
    head_.set_text(head);
    n_dest_ = n.n_dest < core::state::Nav::kMaxDest ? n.n_dest : core::state::Nav::kMaxDest;
    const core::route::Point here{static_cast<int32_t>(std::lround(f.gnss.lat * 1e7)),
                                  static_cast<int32_t>(std::lround(f.gnss.lon * 1e7))};
    // No map, no rows: nothing to pick and nothing to switch.
    rows_ = ph == Phase::NoMap ? 0 : static_cast<uint8_t>(n_dest_ + 3);
    if (rows_) table_.set_row(0, "Where to?", "search");
    for (uint8_t i = 0; i < n_dest_ && rows_; ++i) {
        const core::state::NavDest& d = n.dest[i];
        kind_[i] = d.kind;
        waypoint_[i] = d.waypoint;
        char right[16] = "";
        if (f.gnss.fix) {
            char num[12];
            const char* unit = "";
            nav_distance(num, sizeof num, &unit,
                         core::route::dist_m(here, core::route::Point{d.lat_e7, d.lon_e7}) * 3.28084, units());
            std::snprintf(right, sizeof right, "%s %s", num, unit);
        }
        table_.set_row(i + 1u, d.name, right);
    }
    if (rows_) {
        char lim[12] = "off";
        if (n.max_mph) std::snprintf(lim, sizeof lim, "%u mph", unsigned(n.max_mph));
        table_.set_row(n_dest_ + 1u, "Speed limit", lim);
        table_.set_row(n_dest_ + 2u, "Dirt roads", n.avoid_dirt ? "avoid" : "allowed");
    }
    table_.set_row_count(rows_);
    table_.set_focus(rows_ ? (focus_ < rows_ ? focus_ : 0) : -1);
    const bool on_dest = focus_ >= 1 && focus_ <= n_dest_;
    phint_.set_text(ph == Phase::NoMap ? ""
                    : focus_ == 0 ? "C/D pick   E spell an address, town or place"
                    : on_dest && kind_[focus_ - 1] == core::state::NavDest::Waypoint
                        ? "C/D pick   E go   hold E put this waypoint here"
                    : focus_ == n_dest_ + 1 ? "C/D pick   E up   hold E down"
                    : focus_ == n_dest_ + 2 ? "C/D pick   E switch"
                                            : "C/D pick   E go");
}

void NavScreen::update_search(const Frame& f) {
    const core::state::Nav& n = f.nav;
    const bool number = n.search == core::state::Nav::SearchNumber;
    // No index on the card: nothing can match, so every key would be grey
    // and C/D would have nowhere to go. Say so where the eye lands.
    const bool no_index = !number && !n.search_ok && !n.searching;
    char q[48];
    if (number) std::snprintf(q, sizeof q, "%.18s #%s_", n.street, n.digits);
    else if (no_index) std::snprintf(q, sizeof q, "No search index on card");
    else std::snprintf(q, sizeof q, "Where to? %s_", n.query);
    squery_.set_text(q);

    // What the keys can do. Typing: DEL once there is something to
    // delete, LIST once there are results, and the letters that lead
    // somewhere. A number: DEL (past the first digit, back to the street),
    // GO, and the digits (no leading 0).
    uint64_t en = 0;
    if (number) {
        en = 3;
        for (int dgt = 0; dgt < 10; ++dgt)
            if (dgt || n.digits[0]) en |= uint64_t(1) << (2 + 26 + dgt);
    } else {
        if (n.query[0]) en |= 1;
        if (n.n_hits) en |= 2;
        en |= (n.next & ((uint64_t(1) << core::route::kAlphabetLen) - 1)) << 2;
    }
    enabled_ = en;
    n_hits_ = number ? 0 : n.n_hits;
    if (in_list_ && n_hits_ == 0) in_list_ = false;
    place_cursor();
    keys_.set(en, in_list_ ? -1 : cursor_, number ? "GO" : "LIST");

    char st[64];
    if (no_index)
        std::snprintf(st, sizeof st, "Copy route/search.idx to the card (build_maps.sh)");
    else if (number)
        std::snprintf(st, sizeof st, "House number, or GO for the street itself");
    else if (n.searching)
        std::snprintf(st, sizeof st, "Looking...");
    else if (!n.query[0])
        std::snprintf(st, sizeof st, "Spell a street, town or place, or a house number first");
    else if (std::strspn(n.query, "0123456789") == std::strlen(n.query) && !n.n_hits)
        std::snprintf(st, sizeof st, "House number %.6s: now spell the street", n.query);
    else if (n.matches == 0)
        std::snprintf(st, sizeof st, "Nothing by that name");
    else if (!n.n_hits)
        std::snprintf(st, sizeof st, "%u matches: keep spelling", unsigned(n.matches));
    else
        std::snprintf(st, sizeof st, "%u matches, nearest first", unsigned(n.matches));
    sstatus_.set_text(st);

    for (uint8_t i = 0; i < n_hits_; ++i) {
        const core::state::Nav::Hit& h = n.hits[i];
        char num[12];
        const char* unit = "";
        nav_distance(num, sizeof num, &unit, h.dist_mi * 5280.0, units());
        char right[32];   // the table keeps 13 characters of it
        std::snprintf(right, sizeof right, "%s%s %.6s", num, unit, h.city);
        table_.set_row(i, h.name, right);
    }
    table_.set_row_count(n_hits_);
    table_.set_focus(in_list_ ? (focus_ < n_hits_ ? focus_ : 0) : -1);
    phint_.set_text(no_index ? "hold E: back"
                    : in_list_ ? "C/D pick  E go  hold E: keys"
                    : number ? "C/D move  E press  GO: go  hold E: close"
                             : "C/D move  E press  LIST: results  hold E: close");
}

void NavScreen::update_guide(const Frame& f) {
    using core::route::Phase;
    const core::state::Nav& n = f.nav;
    const Phase ph = static_cast<Phase>(n.phase);
    const bool live = n.following && ph != Phase::Routing && ph != Phase::NoSlowRoute;
    arrow_.set(n.cue_kind, n.cue_turn_deg, live);
    char num[12] = "--";
    const char* unit = "";
    if (live) nav_distance(num, sizeof num, &unit, n.to_cue_ft, units());
    dist_.set_text(num);
    dist_unit_.set_text(unit);
    char road[24];
    std::snprintf(road, sizeof road, "%.19s", live ? n.cue_road : n.target);
    road_.set_text(road);
    char lim[8] = "";
    if (live && n.limit_mph) std::snprintf(lim, sizeof lim, "%u", unsigned(n.limit_mph));
    limit_cap_.set_text(live && n.limit_mph ? "LIMIT" : "");
    limit_.set_text(lim);

    char remain[64] = "";
    if (n.following) {
        char d[12];
        const char* du = "";
        nav_distance(d, sizeof d, &du, n.remain_mi * 5280.0, units());
        // The arrival time on the clock, as a Garmin shows it, when the
        // GPS has told us the time.
        char eta[12] = "";
        if (f.gnss.utc_ms != 0 || f.gnss.fix) {
            const bool have = has_ctx() && ctx().settings;
            const uint32_t at = static_cast<uint32_t>((uint64_t(f.gnss.utc_ms) + uint64_t(n.remain_s) * 1000u) % 86400000u);
            u::fmt_clock(eta, sizeof eta, at, have ? ctx().settings->tz_offset_h : 0, true,
                         have && ctx().settings->clock_12h);
        }
        // 37 characters fit: "12.4 mi  18min  arr 2:35  Little Roc".
        std::snprintf(remain, sizeof remain, "%s %s  %umin%s%s  %.10s", d, du,
                      unsigned((n.remain_s + 59) / 60), eta[0] ? "  arr " : "", eta, n.target);
    }
    remain_.set_text(remain);

    char status[48] = "";
    const char* hint = show_map_ ? "E turns   hold E stop" : "E route   hold E stop";
    switch (ph) {
        case Phase::Routing:
            std::snprintf(status, sizeof status, "Routing to %s...", n.target);
            break;
        case Phase::Rerouting:
            std::snprintf(status, sizeof status, "Off route: rerouting...");
            break;
        case Phase::NoSlowRoute:
            std::snprintf(status, sizeof status, "No route under %u mph", unsigned(n.max_mph));
            hint = "E any speed   hold E stop";
            break;
        case Phase::Arrived:
            std::snprintf(status, sizeof status, "Arrived: %s", n.target);
            break;
        default:
            if (n.off_route) std::snprintf(status, sizeof status, "Off route");
            else if (n.over_limit) std::snprintf(status, sizeof status, "Road over %u mph", unsigned(n.max_mph));
            else pace_line(status, sizeof status, n, units());
            break;
    }
    status_.set_text(status);
    hint_.set_text(hint);
}

bool NavScreen::handle_search(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::state::Command;
    using core::state::CommandId;
    const bool number = search_ == core::state::Nav::SearchNumber;
    const bool hold = e.press != core::nav::Press::Short;
    if (in_list_) {
        if (e.button != Button::E) return false;          // C/D: nav moves through the results
        if (hold) {                          // back to the keys
            in_list_ = false;
            return true;
        }
        if (focus_ >= n_hits_) return true;
        const int32_t pick = focus_;
        in_list_ = false;
        focus_ = 0;
        reset_focus_ = 0;
        return post(Command(CommandId::NavSearchPick, pick));
    }
    if (e.button == Button::C || e.button == Button::D) {
        // The next key on offer that way, round the grid.
        if (enabled_ == 0) return true;
        const int step = e.button == Button::D ? 1 : -1;
        int k = cursor_ < 0 ? 0 : cursor_;
        for (int i = 0; i < KeyGrid::kKeys; ++i) {
            k = (k + step + KeyGrid::kKeys) % KeyGrid::kKeys;
            if ((enabled_ >> k) & 1) break;
        }
        cursor_ = k;
        keys_.set(enabled_, cursor_, number ? "GO" : "LIST");
        return true;
    }
    if (e.button != Button::E) return false;
    if (hold) return post(Command(CommandId::NavSearch, int32_t{0}));
    const int k = cursor_;
    if (k < 0 || !((enabled_ >> k) & 1)) return true;
    if (k == 0) return post(Command(CommandId::NavSearchKey, int32_t{8}));
    if (k == 1) {
        if (number) return post(Command(CommandId::NavSearchGo));
        in_list_ = true;                     // LIST: the cursor to the results
        focus_ = 0;
        reset_focus_ = 0;
        return true;
    }
    // The cursor stays on this key; place_cursor() moves it only if the
    // next keys on offer leave it out.
    return post(Command(CommandId::NavSearchKey, static_cast<int32_t>(KeyGrid::key_char(k))));
}

bool NavScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    using core::route::Phase;
    using core::state::Command;
    using core::state::CommandId;
    if (view_ == View::Search) return handle_search(e);
    if (e.button != Button::E) return false;
    const bool hold = e.press != Press::Short;
    if (view_ == View::Guide) {
        switch (static_cast<Phase>(phase_)) {
            case Phase::NoSlowRoute:
                return post(Command(hold ? CommandId::NavCancel : CommandId::NavAnySpeed));
            default:
                if (hold) return post(Command(CommandId::NavCancel));
                want_map_ = !want_map_;      // the route view, or back to the turns
                return true;
        }
    }
    if (rows_ == 0) return false;
    if (focus_ == 0) {
        if (!hold) post(Command(CommandId::NavSearch, int32_t{1}));
        return true;
    }
    // Settings, so a ConfigSet: `logic` is their one writer. The limit
    // steps up 5 mph (50 ... 75, off) on E and down on hold E, round the ends.
    if (focus_ == n_dest_ + 1) {
        const uint8_t n = core::config::kNavLimitCount;
        const uint8_t next = static_cast<uint8_t>((limit_ix_ + (hold ? n - 1 : 1)) % n);
        return post(Command::config_set(kSpeedLimitKey, static_cast<float>(next)));
    }
    if (focus_ == n_dest_ + 2)
        return post(Command::config_set(kAvoidDirtKey, avoid_dirt_ ? 0.0f : 1.0f));
    if (focus_ > n_dest_ + 2) return false;
    const uint8_t i = static_cast<uint8_t>(focus_ - 1);
    if (hold) {
        // `logic` decides whether the fix is good enough, as for the lap line.
        // Taken on any row, as the old NAV did: a long E nav got would
        // open the overlay menu instead.
        if (kind_[i] == core::state::NavDest::Waypoint)
            post(Command(CommandId::NavDropWaypoint, static_cast<int32_t>(waypoint_[i])));
        return true;
    }
    return post(Command(CommandId::NavGo, static_cast<int32_t>(i)));
}

WidgetList NavScreen::widgets() { return WidgetList{list_, n_list_}; }

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

    map_.set_live(f.gnss.fix);
}

WidgetList TripMapScreen::widgets() { return WidgetList{list_, 4}; }

}  // namespace ui
