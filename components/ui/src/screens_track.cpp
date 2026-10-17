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
    phase_.configure(phase, f.lap.phase == Phase::Timing ? color::kGood : color::kWarn,
                     1, Align::Left);

    char buf[24];
    u::fmt_duration(buf, sizeof buf, f.lap.current_ms,
                    f.lap.phase == Phase::Timing);
    current_.set_text(buf);

    // A new best lap flashes for five seconds, as the factory dashes do: the
    // one moment worth a glance away from the track.
    char line[24];
    u::fmt_duration(line, sizeof line, f.lap.last_ms, f.lap.count > 0);
    const uint32_t since_lap = f.t_ms - f.lap.last_done_ms;
    const bool flash = f.lap.last_was_best && f.lap.last_done_ms != 0 && since_lap < kShowMs;
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
    const uint32_t since_split = f.t_ms - f.lap.split_at_ms;
    char title[16];
    if (f.lap.split_ms != 0 && since_split < kShowMs) {
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

}  // namespace ui
