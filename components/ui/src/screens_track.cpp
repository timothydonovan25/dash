#include "ui/screens.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"
#include <cstdio>
#include <initializer_list>

namespace ui {

using core::state::Frame;
namespace u = core::units;

// --- LapScreen ----------------------------------------------------------
//
// Read at 60 mph between corners, so: the running lap is the biggest thing
// on it, the delta is a colour before it is a number, and the best lap sits
// where a glance finds it without hunting.
namespace {
// 480x320. The running lap owns the top half at a size readable mid-corner;
// the delta sits directly under it, because those two are read together and
// anything between them is a distraction. Reference times go down the right.
constexpr Rect kLapPhase   { 14,   6, 300,  14};
constexpr Rect kLapCurrent { 10,  34, 300,  74};
constexpr Rect kLapDelta   { 14, 122, 292,  76};
// Down the right, four of them: what the running lap is measured against.
constexpr Rect kLapLast    {322,  36, 144,  60};
constexpr Rect kLapBest    {322, 105, 144,  60};
constexpr Rect kLapIdeal   {322, 174, 144,  60};
constexpr Rect kLapCount   {322, 243, 144,  60};
// Along the bottom, speed and the last split.
constexpr Rect kLapSpeed   { 14, 218, 140,  86};
constexpr Rect kLapSplit   {162, 218, 144,  86};
// How long a split, or a new best lap, stays up.
constexpr uint32_t kShowMs = 5000;
}  // namespace

void LapScreen::enter(Canvas&) {
    phase_.set_bounds(kLapPhase);
    phase_.configure("NO LINE", color::kDim, 1, Align::Left);

    // Inset: a widget clears its own bounds, so bounds matching the
    // tile would wipe the frame drawn under it.
    current_.set_bounds(kLapCurrent.inset(6));
    // Scale 6: eight characters are 281 px, inside the tile's 288. At 7
    // they were 329 and ran over both sides of it.
    current_.configure("--:--.--", color::kText, 6, Align::Center);

    delta_.set_bounds(kLapDelta);

    for (Label* l : {&last_, &best_, &ideal_}) {
        const Rect& t = l == &last_ ? kLapLast : l == &best_ ? kLapBest : kLapIdeal;
        l->set_bounds(Rect{static_cast<int16_t>(t.x + 8), static_cast<int16_t>(t.y + 26),
                           static_cast<int16_t>(t.w - 16), 20});
    }
    last_.configure("--:--.--", color::kText, 2, Align::Left);
    best_.configure("--:--.--", color::kAccent, 2, Align::Left);
    ideal_.configure("--:--.--", color::kDim, 2, Align::Left);

    lap_count_.set_bounds(kLapCount.inset(7));
    lap_count_.configure("LAPS", "", 0, 3);

    speed_.set_bounds(kLapSpeed.inset(8));
    speed_.configure("SPEED", "mph", 0, 4);

    split_title_.set_bounds(Rect{static_cast<int16_t>(kLapSplit.x + 8),
                                 static_cast<int16_t>(kLapSplit.y + 8),
                                 static_cast<int16_t>(kLapSplit.w - 16), 10});
    split_title_.configure("SPLITS", theme::secondary, 1, Align::Left);
    split_.set_bounds(Rect{static_cast<int16_t>(kLapSplit.x + 8),
                           static_cast<int16_t>(kLapSplit.y + 34),
                           static_cast<int16_t>(kLapSplit.w - 16), 30});
    split_.configure("--", color::kDim, 3, Align::Center);

    list_[0] = &phase_;   list_[1] = &current_; list_[2] = &delta_;
    list_[3] = &last_;    list_[4] = &best_;    list_[5] = &lap_count_;
    list_[6] = &speed_;   list_[7] = &ideal_;   list_[8] = &split_title_;
    list_[9] = &split_;
}

void LapScreen::update(const Frame& f) {
    using core::lap::Phase;

    const char* phase = "NO LINE";
    switch (f.lap.phase) {
        case Phase::NoLine: phase = "NO LINE  -  hold E to set"; break;
        case Phase::Armed:  phase = "ARMED";  break;
        case Phase::Timing: phase = "TIMING"; break;
    }
    // Which saved track the line is (core::track), and its record: a line
    // that came back by itself says so, or a rider who set nothing would
    // wonder where it came from.
    char status[48];
    size_t k = 0;
    for (const char* p = phase; *p && k + 1 < sizeof status; ++p) status[k++] = *p;
    status[k] = '\0';
    if (f.lap.track >= 0 && f.lap.phase != Phase::NoLine) {
        char rec[16];
        u::fmt_duration(rec, sizeof rec, f.lap.track_best_ms, true);
        if (f.lap.track_best_ms > 0) {
            std::snprintf(status + k, sizeof status - k, "   TRACK %d%s   REC %s",
                          f.lap.track + 1, f.lap.track_recalled ? " (saved)" : "", rec);
        } else {
            std::snprintf(status + k, sizeof status - k, "   TRACK %d%s", f.lap.track + 1,
                          f.lap.track_recalled ? " (saved)" : "");
        }
    }
    phase_.configure(status, f.lap.phase == Phase::Timing ? color::kGood : color::kWarn,
                     1, Align::Left);

    char buf[24];
    u::fmt_duration(buf, sizeof buf, f.lap.current_ms,
                    f.lap.phase == Phase::Timing);
    current_.set_text(buf);

    // A new best lap flashes for five seconds, as the factory dashes do: the
    // one moment worth a glance away from the track.
    char line[24];
    u::fmt_duration(line, sizeof line, f.lap.last_ms, f.lap.count > 0);
    // Signed: the lap slot can be newer than the frame's clock.
    const int32_t since_lap = static_cast<int32_t>(f.t_ms - f.lap.last_done_ms);
    const bool flash = f.lap.last_was_best && f.lap.last_done_ms != 0 &&
                       since_lap < static_cast<int32_t>(kShowMs);
    const bool lit = !flash || (since_lap / 250) % 2 == 0;
    last_.configure(line, flash ? (lit ? color::kGood : color::kBg) : color::kText, 2,
                    Align::Left);

    u::fmt_duration(line, sizeof line, f.lap.best_ms, f.lap.best_ms > 0);
    best_.set_text(line);

    // The best of each sector added up: the lap the rider has already shown
    // they can do, a piece at a time.
    u::fmt_duration(line, sizeof line, f.lap.ideal_ms, f.lap.ideal_ms > 0);
    ideal_.set_text(line);

    // The logic task says whether the delta means anything: there has to be
    // a reference lap, and the rider has to be on it. Inferring it from
    // `delta_ms != 0` would show `--` to a rider who is exactly on their
    // best lap, which is the one moment they most want to see the zero.
    delta_.set(f.lap.delta_ms, f.lap.delta_valid);

    lap_count_.set(static_cast<float>(f.lap.count), true);

    const char* sunit = "mph";
    speed_.set(u::speed_for_display(f.motion.best_mph, units(), &sunit),
               f.gnss.fix);
    speed_.set_unit(sunit);

    // The last split, for five seconds after it: that sector against its
    // best, green for quicker. Otherwise which sector this is, or that the
    // splits are still to be placed (the first clean lap does it).
    const int32_t since_split = static_cast<int32_t>(f.t_ms - f.lap.split_at_ms);   // signed, as above
    char title[16];
    if (f.lap.split_ms != 0 && since_split < static_cast<int32_t>(kShowMs)) {
        std::snprintf(title, sizeof title, "SECTOR %u", f.lap.split_sector + 1u);
        split_title_.configure(title, theme::secondary, 1, Align::Left);
        if (f.lap.split_delta_valid) {
            u::fmt_delta(line, sizeof line, f.lap.split_delta_ms, true, 2);
            split_.configure(line, f.lap.split_delta_ms <= 0 ? color::kGood : color::kWarn,
                             3, Align::Center);
        } else {
            u::fmt_duration(line, sizeof line, f.lap.split_ms, true);
            split_.configure(line, color::kText, 2, Align::Center);
        }
    } else if (f.lap.sector >= 0) {
        std::snprintf(title, sizeof title, "SECTOR %d", f.lap.sector + 1);
        split_title_.configure(title, theme::secondary, 1, Align::Left);
        split_.configure("--", color::kDim, 3, Align::Center);
    } else {
        split_title_.configure("SPLITS", theme::secondary, 1, Align::Left);
        split_.configure(f.lap.phase == Phase::NoLine ? "--" : "LAP 1",
                         color::kDim, f.lap.phase == Phase::NoLine ? 3 : 2, Align::Center);
    }
}

bool LapScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    using core::state::Command;
    using core::state::CommandId;

    if (e.button != Button::E) return false;
    if (e.press == Press::Long) {
        // Set the line where the bike is standing. The logic task fills in
        // the position and heading from the current fix — the screen has a
        // Frame, not a receiver, and must not be the thing that decides
        // whether the fix is good enough.
        return post(Command(CommandId::LapSetLine));
    }
    return post(Command(CommandId::LapArm));
}

void LapScreen::draw_background(Canvas& c) {
    c.draw_round_rect(kLapCurrent, chrome::kRadius, theme::rule);
    for (const Rect& r : {kLapLast, kLapBest, kLapIdeal, kLapCount, kLapSpeed, kLapSplit}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
    c.draw_text(kLapLast.x + 8, kLapLast.y + 8, "LAST", theme::secondary, 1,
                Align::Left);
    c.draw_text(kLapBest.x + 8, kLapBest.y + 8, "BEST", theme::secondary, 1,
                Align::Left);
    c.draw_text(kLapIdeal.x + 8, kLapIdeal.y + 8, "IDEAL", theme::secondary, 1,
                Align::Left);
    // The delta has no frame on purpose: it is the one thing on this screen
    // read by colour and position rather than by reading a box.
}

WidgetList LapScreen::widgets() { return WidgetList{list_, 10}; }

// --- LapHistoryScreen ---------------------------------------------------
namespace {
constexpr Rect kHistTitle { 14,  30, 452,  14};
constexpr Rect kHistTable { 14,  52, 452, 256};
}  // namespace

void LapHistoryScreen::enter(Canvas&) {
    table_.set_bounds(kHistTable);
    table_.configure(10, 2);
    list_[0] = &table_;
}

void LapHistoryScreen::set_history(const uint32_t* laps_ms, size_t count,
                                   uint32_t best_ms, const core::state::LapPeak* peaks) {
    laps_ = laps_ms;
    lap_count_ = count;
    best_ms_ = best_ms;
    peaks_ = peaks;
}

void LapHistoryScreen::update(const Frame& f) {
    const size_t count = laps_ ? lap_count_ : 0;
    const uint32_t best = best_ms_ ? best_ms_ : f.lap.best_ms;
    if (shown_ && count == shown_count_ && best == shown_best_ && peaks_ == shown_peaks_) return;
    shown_ = true;
    shown_count_ = count;
    shown_best_ = best;
    shown_peaks_ = peaks_;

    if (count == 0) {
        table_.clear_rows();
        table_.set_row(0, "no laps yet", "");
        return;
    }

    // Newest first: the lap a rider wants is the one they just did.
    // Left: the lap, its time and its gap to the best. Right: how hard it
    // was ridden -- top speed, top rpm, deepest lean -- which is what says
    // why one lap was quicker than another.
    const size_t rows = lap_count_ < Table::kRows ? lap_count_ : Table::kRows;
    for (size_t i = 0; i < rows; ++i) {
        const size_t idx = lap_count_ - 1 - i;
        char left[24];
        size_t k = 0;
        left[k++] = 'L';
        char num[8];
        u::fmt_float(num, sizeof num, static_cast<float>(idx + 1), 0);
        for (const char* p = num; *p && k + 1 < sizeof left; ++p) left[k++] = *p;
        left[k++] = ' ';
        k += u::fmt_duration(left + k, sizeof left - k, laps_[idx], true);
        if (best > 0 && k + 2 < sizeof left) {
            // Two decimals here: this is one measured lap time against
            // another, not a position-matched estimate, and the crossing
            // interpolation is good to a few milliseconds.
            left[k++] = ' ';
            k += u::fmt_delta(left + k, sizeof left - k,
                              static_cast<int32_t>(laps_[idx]) - static_cast<int32_t>(best),
                              true, 2);
        }
        left[k] = '\0';

        char right[32] = "";
        if (peaks_) {
            const core::state::LapPeak& pk = peaks_[idx];
            const char* unit = "mph";
            char spd[8], rpm[8], lean[8];
            u::fmt_float(spd, sizeof spd, u::speed_for_display(pk.mph, units(), &unit), 0);
            u::fmt_float(rpm, sizeof rpm, pk.rpm / 1000.0f, 1);
            u::fmt_float(lean, sizeof lean, pk.lean_deg, 0);
            std::snprintf(right, sizeof right, "%3s %4sk %2s", spd, rpm, lean);
        }
        table_.set_row(i, left, right);
    }
    table_.set_row_count(rows);
}

void LapHistoryScreen::draw_background(Canvas& c) {
    // Over the columns they name. The table is scale 2, 12 px a character:
    // "L5 1:30.10 +0.00" on the left, " 63 11.2k 44" right-aligned. These
    // used to be a full-width title label, which cleared its bounds and
    // wiped the right-hand heading drawn under it.
    constexpr int16_t kCh = 12;
    const int16_t y = kHistTitle.y;
    const int16_t l = kHistTitle.x;
    const int16_t r = kHistTitle.right();
    c.draw_text(l, y, "LAP", theme::secondary, 1, Align::Left);
    c.draw_text(static_cast<int16_t>(l + 3 * kCh), y, "TIME", theme::secondary, 1, Align::Left);
    c.draw_text(static_cast<int16_t>(l + 11 * kCh), y, "VS BEST", theme::secondary, 1, Align::Left);
    c.draw_text(static_cast<int16_t>(r - 9 * kCh), y, "TOP SPD", theme::secondary, 1, Align::Right);
    c.draw_text(static_cast<int16_t>(r - 3 * kCh), y, "RPM", theme::secondary, 1, Align::Right);
    c.draw_text(r, y, "LEAN", theme::secondary, 1, Align::Right);
}

WidgetList LapHistoryScreen::widgets() { return WidgetList{list_, 1}; }

// --- TrackMapScreen -----------------------------------------------------
namespace {
constexpr Rect kTrkMap  { 14,  32, 286, 276};
constexpr Rect kTrkLap  {314,  44, 152,  72};
constexpr Rect kTrkLine {314, 138, 152,  72};
constexpr Rect kTrkDelta{314, 232, 152,  72};
}  // namespace

void TrackMapScreen::enter(Canvas&) {
    map_.set_bounds(kTrkMap);
    // "--:--.--" at scale 3 is 141 px. At 136 wide its last digit drew past
    // the bounds, where no partial update ever reaches, and went stale.
    lap_.set_bounds(Rect{320, 70, 144, 24});
    lap_.configure("--:--.--", color::kText, 3, Align::Left);
    to_line_.set_bounds(kTrkLine.inset(7));
    to_line_.configure("TO LINE", "ft", 0, 3);
    delta_.set_bounds(kTrkDelta.inset(6));
    list_[0] = &map_;
    list_[1] = &lap_;
    list_[2] = &to_line_;
    list_[3] = &delta_;
}

void TrackMapScreen::draw_background(Canvas& c) {
    for (const Rect& r : {kTrkLap, kTrkLine, kTrkDelta}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
    c.draw_text(kTrkLap.x + 8, kTrkLap.y + 8, "LAP", theme::secondary, 1,
                Align::Left);
}

void TrackMapScreen::update(const Frame& f) {
    char buf[16];
    u::fmt_duration(buf, sizeof buf, f.lap.current_ms,
                    f.lap.phase == core::lap::Phase::Timing);
    lap_.set_text(buf);

    to_line_.set(f.lap.dist_to_line_ft,
                 f.gnss.fix && f.lap.phase != core::lap::Phase::NoLine);
    delta_.set(f.lap.delta_ms, f.lap.delta_valid);
    map_.set_live(f.gnss.fix);
}

WidgetList TrackMapScreen::widgets() { return WidgetList{list_, 4}; }

// --- RaceScreen ---------------------------------------------------------
//
// Read in a glance down the straight. The bar is read by length and colour,
// the gear by shape, the delta by colour; only the lap times are read as
// numbers, and those are the ones a rider looks at between corners.
namespace {
constexpr Rect kRaceRev    {  6,   6, 468,  50};
constexpr Rect kRaceGear   {  6,  66, 156, 248};
constexpr Rect kRaceLap    {170,  66, 304,  70};
constexpr Rect kRaceDelta  {170, 144, 304,  72};
constexpr Rect kRaceLast   {170, 224, 150,  90};
constexpr Rect kRaceBest   {324, 224, 150,  90};
// The right-hand end of the bar: a thousand past the flash, on a thousand,
// so the lamps' whole range is on it and the redline is not the last pixel.
uint16_t race_full_rpm(const core::config::Settings* st) {
    const uint32_t flash = st ? st->shift_flash_rpm : 8800;
    uint32_t full = ((flash + 1000 + 999) / 1000) * 1000;
    if (full > 16000) full = 16000;
    return static_cast<uint16_t>(full);
}
}  // namespace

void RaceScreen::enter(Canvas&) {
    rev_.set_bounds(kRaceRev);
    rev_.show_number(true);   // the rpm as a number, over the bar
    gear_.set_bounds(Rect{static_cast<int16_t>(kRaceGear.x + 6), static_cast<int16_t>(kRaceGear.y + 20),
                          static_cast<int16_t>(kRaceGear.w - 12), 156});
    // Scale 20 is 100x140 a glyph: inside the tile, and the biggest thing
    // on the panel, because the gear is what a rider checks most.
    gear_.configure("-", color::kDim, 20, Align::Center);
    status_.set_bounds(Rect{static_cast<int16_t>(kRaceGear.x + 6),
                            static_cast<int16_t>(kRaceGear.bottom() - 26),
                            static_cast<int16_t>(kRaceGear.w - 12), 14});
    status_.configure("NO LINE", color::kWarn, 1, Align::Center);
    current_.set_bounds(kRaceLap.inset(6));
    current_.configure("--:--.--", color::kText, 6, Align::Center);
    delta_.set_bounds(kRaceDelta);
    for (Label* l : {&last_, &best_}) {
        const Rect& t = l == &last_ ? kRaceLast : kRaceBest;
        // Scale 3: "--:--.--" is 144 px, the tile less its frame.
        l->set_bounds(Rect{static_cast<int16_t>(t.x + 3), static_cast<int16_t>(t.y + 40),
                           static_cast<int16_t>(t.w - 6), 22});
    }
    last_.configure("--:--.--", color::kText, 3, Align::Center);
    best_.configure("--:--.--", color::kAccent, 3, Align::Center);
    list_[0] = &rev_;     list_[1] = &gear_;  list_[2] = &status_;
    list_[3] = &current_; list_[4] = &delta_; list_[5] = &last_;
    list_[6] = &best_;
}

const char* RaceScreen::widget_name(size_t i) const {
    static const char* const kNames[] = {"rev", "gear", "status", "lap", "delta", "last", "best"};
    return i < sizeof kNames / sizeof kNames[0] ? kNames[i] : nullptr;
}

void RaceScreen::update(const Frame& f) {
    using core::lap::Phase;
    const core::config::Settings* st = has_ctx() ? ctx().settings : nullptr;

    rev_.set_scale(race_full_rpm(st));
    rev_.set(f.engine.rpm, f.engine.shift_mask != 0, f.engine.shift_flash,
             f.engine.rpm > 0.0f || f.engine.neutral);
    rev_.set_peak(f.engine.rpm_peak);   // the rpm before the shift, held a moment

    // As SPEED has it: neutral is sensed, the gear inferred, and a gear
    // nobody can vouch for is a dash rather than a stale digit.
    char g[2] = {'-', '\0'};
    Color gc = color::kDim;
    if (f.engine.neutral) {
        g[0] = 'N';
        gc = color::kGood;
    } else if (f.engine.gear > 0 && f.engine.gear <= 9) {
        g[0] = static_cast<char>('0' + f.engine.gear);
        gc = color::kText;
    }
    gear_.configure(g, gc, 20, Align::Center);

    char buf[24];
    switch (f.lap.phase) {
        case Phase::NoLine:
            status_.configure("NO LINE - HOLD E", color::kWarn, 1, Align::Center);
            break;
        case Phase::Armed:
            status_.configure(f.lap.track_recalled ? "ARMED - SAVED LINE" : "ARMED",
                              color::kWarn, 1, Align::Center);
            break;
        case Phase::Timing:
            std::snprintf(buf, sizeof buf, "LAP %u", f.lap.count + 1u);
            status_.configure(buf, color::kGood, 1, Align::Center);
            break;
    }

    u::fmt_duration(buf, sizeof buf, f.lap.current_ms, f.lap.phase == Phase::Timing);
    current_.set_text(buf);
    delta_.set(f.lap.delta_ms, f.lap.delta_valid);

    // A new best flashes, as on LAP.
    u::fmt_duration(buf, sizeof buf, f.lap.last_ms, f.lap.count > 0);
    const int32_t since = static_cast<int32_t>(f.t_ms - f.lap.last_done_ms);   // signed, as on LAP
    const bool flash = f.lap.last_was_best && f.lap.last_done_ms != 0 &&
                       since < static_cast<int32_t>(kShowMs);
    const bool lit = !flash || (since / 250) % 2 == 0;
    last_.configure(buf, flash ? (lit ? color::kGood : color::kBg) : color::kText, 3,
                    Align::Center);

    // The session's best once there is one; before that the track's record,
    // dimmed, so the first lap of the day has a number to chase.
    if (f.lap.best_ms > 0) {
        u::fmt_duration(buf, sizeof buf, f.lap.best_ms, true);
        best_.configure(buf, color::kAccent, 3, Align::Center);
    } else {
        u::fmt_duration(buf, sizeof buf, f.lap.track_best_ms, f.lap.track_best_ms > 0);
        best_.configure(buf, color::kDim, 3, Align::Center);
    }
}

bool RaceScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    using core::state::Command;
    using core::state::CommandId;
    if (e.button != Button::E) return false;
    return post(Command(e.press == Press::Long ? CommandId::LapSetLine : CommandId::LapArm));
}

void RaceScreen::draw_background(Canvas& c) {
    for (const Rect& r : {kRaceGear, kRaceLap, kRaceLast, kRaceBest}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
    c.draw_text(static_cast<int16_t>(kRaceGear.x + 8), static_cast<int16_t>(kRaceGear.y + 8),
                "GEAR", theme::secondary, 1, Align::Left);
    c.draw_text(static_cast<int16_t>(kRaceLast.x + 8), static_cast<int16_t>(kRaceLast.y + 8),
                "LAST", theme::secondary, 1, Align::Left);
    c.draw_text(static_cast<int16_t>(kRaceBest.x + 8), static_cast<int16_t>(kRaceBest.y + 8),
                "BEST", theme::secondary, 1, Align::Left);
}

WidgetList RaceScreen::widgets() { return WidgetList{list_, 7}; }

// --- SessionScreen ------------------------------------------------------
namespace {
constexpr Rect kSessTrack{14, 30, 452, 12};
// Three rows of three. Each tile's caption is chrome; its value a label.
constexpr int16_t kSessX[3] = {14, 168, 322};
constexpr int16_t kSessY[3] = {52, 140, 228};
constexpr int16_t kSessW = 144;
constexpr int16_t kSessH = 80;
constexpr const char* kSessCaption[9] = {
    "BEST LAP", "IDEAL LAP", "LAPS",
    "TOP SPEED", "DISTANCE", "RIDING TIME",
    "LEAN L / R", "BEST AIR", "AVG SPEED",
};
Rect sess_tile(int i) { return Rect{kSessX[i % 3], kSessY[i / 3], kSessW, kSessH}; }

// Hours and minutes past an hour, minutes and seconds below it.
void fmt_ride_time(char* buf, size_t n, uint32_t s) {
    if (s >= 3600) {
        std::snprintf(buf, n, "%u:%02u:%02u", static_cast<unsigned>(s / 3600),
                      static_cast<unsigned>(s / 60 % 60), static_cast<unsigned>(s % 60));
    } else {
        std::snprintf(buf, n, "%u:%02u", static_cast<unsigned>(s / 60),
                      static_cast<unsigned>(s % 60));
    }
}
}  // namespace

void SessionScreen::enter(Canvas&) {
    track_.set_bounds(kSessTrack);
    track_.configure("", theme::secondary, 1, Align::Left);
    list_[0] = &track_;
    for (int i = 0; i < kTiles; ++i) {
        const Rect t = sess_tile(i);
        value_[i].set_bounds(Rect{static_cast<int16_t>(t.x + 4), static_cast<int16_t>(t.y + 34),
                                  static_cast<int16_t>(t.w - 8), 22});
        value_[i].configure("--", color::kDim, 3, Align::Center);
        list_[1 + i] = &value_[i];
    }
}

void SessionScreen::show(int i, const char* text, bool valid) {
    // Scale 3 where it fits, 2 where it does not: "--:--.--" and an hour's
    // riding are wider than a tile at 3.
    const int16_t room = value_[i].bounds.w;
    const uint8_t scale = text_width_px(text, 3) <= room ? 3 : 2;
    value_[i].configure(text, valid ? color::kText : color::kDim, scale, Align::Center);
}

void SessionScreen::update(const Frame& f) {
    char buf[32];
    char line[64];

    // The saved track, when the line is one.
    if (f.lap.track >= 0) {
        char rec[16], ideal[16];
        u::fmt_duration(rec, sizeof rec, f.lap.track_best_ms, f.lap.track_best_ms > 0);
        u::fmt_duration(ideal, sizeof ideal, f.lap.track_ideal_ms, f.lap.track_ideal_ms > 0);
        std::snprintf(line, sizeof line, "TRACK %d   RECORD %s   IDEAL %s", f.lap.track + 1,
                      rec, ideal);
    } else {
        std::snprintf(line, sizeof line, "%s",
                      f.lap.phase == core::lap::Phase::NoLine ? "NO LINE SET" : "TRACK NOT SAVED");
    }
    track_.set_text(line);

    u::fmt_duration(buf, sizeof buf, f.lap.best_ms, f.lap.best_ms > 0);
    show(0, buf, f.lap.best_ms > 0);
    u::fmt_duration(buf, sizeof buf, f.lap.ideal_ms, f.lap.ideal_ms > 0);
    show(1, buf, f.lap.ideal_ms > 0);
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(f.lap.count));
    show(2, buf, f.lap.count > 0);

    const char* unit = "mph";
    char num[12];
    u::fmt_float(num, sizeof num, u::speed_for_display(f.session.top_mph, units(), &unit), 0);
    std::snprintf(buf, sizeof buf, "%s %s", num, unit);
    show(3, buf, f.session.top_mph > 0.0f);

    const char* dunit = "mi";
    u::fmt_float(num, sizeof num, u::distance_for_display(f.session.dist_mi, units(), &dunit), 1);
    std::snprintf(buf, sizeof buf, "%s %s", num, dunit);
    show(4, buf, f.session.dist_mi > 0.0f);

    fmt_ride_time(buf, sizeof buf, f.session.moving_s);
    show(5, buf, f.session.moving_s > 0);

    // Deepest each way, above the 10 mph floor (core::lap::Peaks).
    const bool lean = f.lap.max_lean_left_deg > 0.0f || f.lap.max_lean_right_deg > 0.0f;
    std::snprintf(buf, sizeof buf, "%d / %d", static_cast<int>(f.lap.max_lean_left_deg + 0.5f),
                  static_cast<int>(f.lap.max_lean_right_deg + 0.5f));
    show(6, lean ? buf : "--", lean);

    if (f.session.best_air_ms > 0) {
        u::fmt_float(num, sizeof num, f.session.best_air_ms / 1000.0f, 2);
        std::snprintf(buf, sizeof buf, "%ss x%u", num, static_cast<unsigned>(f.session.jumps));
        show(7, buf, true);
    } else {
        show(7, "--", false);
    }

    unit = "mph";
    u::fmt_float(num, sizeof num, u::speed_for_display(f.session.avg_mph, units(), &unit), 0);
    std::snprintf(buf, sizeof buf, "%s %s", num, unit);
    show(8, buf, f.session.avg_mph > 0.0f);
}

void SessionScreen::draw_background(Canvas& c) {
    for (int i = 0; i < kTiles; ++i) {
        const Rect t = sess_tile(i);
        c.draw_round_rect(t, chrome::kRadius, theme::rule);
        c.draw_text(static_cast<int16_t>(t.x + 8), static_cast<int16_t>(t.y + 8), kSessCaption[i],
                    theme::secondary, 1, Align::Left);
    }
}

WidgetList SessionScreen::widgets() { return WidgetList{list_, 1 + kTiles}; }

}  // namespace ui
