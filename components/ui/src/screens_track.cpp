#include "ui/screens.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"
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
constexpr Rect kLapLast    {322,  40, 144,  56};
constexpr Rect kLapBest    {322, 112, 144,  56};
constexpr Rect kLapCount   {322, 190, 144,  56};
constexpr Rect kLapSpeed   { 14, 218, 292,  86};
}  // namespace

void LapScreen::enter(Canvas&) {
    phase_.set_bounds(kLapPhase);
    phase_.configure("NO LINE", color::kDim, 1, Align::Left);

    // Inset: a widget clears its own bounds, so bounds matching the
    // tile would wipe the frame drawn under it.
    current_.set_bounds(kLapCurrent.inset(6));
    current_.configure("--:--.--", color::kText, 7, Align::Center);

    delta_.set_bounds(kLapDelta);

    last_.set_bounds(Rect{330, 60, 128, 20});
    last_.configure("--:--.--", color::kText, 2, Align::Left);

    best_.set_bounds(Rect{330, 132, 128, 20});
    best_.configure("--:--.--", color::kAccent, 2, Align::Left);

    lap_count_.set_bounds(kLapCount.inset(7));
    lap_count_.configure("LAPS", "", 0, 3);

    speed_.set_bounds(kLapSpeed.inset(8));
    speed_.configure("SPEED", "mph", 0, 5);

    list_[0] = &phase_;   list_[1] = &current_; list_[2] = &delta_;
    list_[3] = &last_;    list_[4] = &best_;    list_[5] = &lap_count_;
    list_[6] = &speed_;
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

    char line[24];
    u::fmt_duration(line, sizeof line, f.lap.last_ms, f.lap.count > 0);
    last_.set_text(line);

    u::fmt_duration(line, sizeof line, f.lap.best_ms, f.lap.best_ms > 0);
    best_.set_text(line);

    // No reference trace yet, so there is no honest delta to draw. The
    // widget shows `--` rather than a zero, which would read as "dead on
    // your best lap" and be a lie (§18: the reference is still an open
    // question).
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
    for (const Rect& r : {kLapLast, kLapBest, kLapCount, kLapSpeed}) {
        c.draw_round_rect(r, chrome::kRadius, theme::rule);
    }
    c.draw_text(kLapLast.x + 8, kLapLast.y + 8, "LAST", theme::secondary, 1,
                Align::Left);
    c.draw_text(kLapBest.x + 8, kLapBest.y + 8, "BEST", theme::secondary, 1,
                Align::Left);
    // The delta has no frame on purpose: it is the one thing on this screen
    // read by colour and position rather than by reading a box.
}

WidgetList LapScreen::widgets() { return WidgetList{list_, 7}; }

// --- LapHistoryScreen ---------------------------------------------------
namespace {
constexpr Rect kHistTitle { 14,  30, 452,  14};
constexpr Rect kHistTable { 14,  52, 452, 256};
}  // namespace

void LapHistoryScreen::enter(Canvas&) {
    title_.set_bounds(kHistTitle);
    title_.configure("LAP AND TIME", color::kDim, 1, Align::Left);
    table_.set_bounds(kHistTable);
    table_.configure(10, 2);
    list_[0] = &title_;
    list_[1] = &table_;
}

void LapHistoryScreen::set_history(const uint32_t* laps_ms, size_t count,
                                   uint32_t best_ms) {
    laps_ = laps_ms;
    lap_count_ = count;
    best_ms_ = best_ms;
}

void LapHistoryScreen::update(const Frame& f) {
    if (!laps_ || lap_count_ == 0) {
        table_.clear_rows();
        table_.set_row(0, "no laps yet", "");
        return;
    }

    // Newest first: the lap a rider wants is the one they just did.
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
        left[k] = '\0';

        char right[16];
        const uint32_t best = best_ms_ ? best_ms_ : f.lap.best_ms;
        if (best > 0) {
            // Two decimals here: this is one measured lap time against
            // another, not a position-matched estimate, and the crossing
            // interpolation is good to a few milliseconds.
            u::fmt_delta(right, sizeof right,
                         static_cast<int32_t>(laps_[idx]) - static_cast<int32_t>(best),
                         true, 2);
        } else {
            right[0] = '\0';
        }
        table_.set_row(i, left, right);
    }
    table_.set_row_count(rows);
}

void LapHistoryScreen::draw_background(Canvas& c) {
    c.draw_text(static_cast<int16_t>(kHistTitle.right()), kHistTitle.y,
                "VS BEST", theme::secondary, 1, Align::Right);
}

WidgetList LapHistoryScreen::widgets() { return WidgetList{list_, 2}; }

// --- TrackMapScreen -----------------------------------------------------
namespace {
constexpr Rect kTrkMap  { 14,  32, 286, 276};
constexpr Rect kTrkLap  {314,  44, 152,  72};
constexpr Rect kTrkLine {314, 138, 152,  72};
constexpr Rect kTrkDelta{314, 232, 152,  72};
}  // namespace

void TrackMapScreen::enter(Canvas&) {
    map_.set_bounds(kTrkMap);
    lap_.set_bounds(Rect{322, 70, 136, 24});
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
    map_.set_position(0.0f, 0.0f, f.gnss.course_deg, f.gnss.fix);
}

WidgetList TrackMapScreen::widgets() { return WidgetList{list_, 4}; }

}  // namespace ui
