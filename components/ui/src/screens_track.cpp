#include "ui/screens.hpp"
#include "core/units/units.hpp"

namespace ui {

using core::state::Frame;
namespace u = core::units;

// --- LapScreen ----------------------------------------------------------
//
// Read at 60 mph between corners, so: the running lap is the biggest thing
// on it, the delta is a colour before it is a number, and the best lap sits
// where a glance finds it without hunting.
namespace {
constexpr Rect kLapPhase   { 16,  24, 288,  16};
constexpr Rect kLapCurrent {  0,  56, 320,  60};
constexpr Rect kLapDelta   { 16, 130, 288,  70};
constexpr Rect kLapLast    {  0, 220, 320,  30};
constexpr Rect kLapBest    {  0, 260, 320,  30};
constexpr Rect kLapCount   { 16, 320, 140,  60};
constexpr Rect kLapSpeed   {166, 320, 140,  60};
}  // namespace

void LapScreen::enter(Canvas&) {
    phase_.set_bounds(kLapPhase);
    phase_.configure("NO LINE", color::kDim, 1, Align::Left);

    current_.set_bounds(kLapCurrent);
    current_.configure("--:--.--", color::kText, 5, Align::Center);

    delta_.set_bounds(kLapDelta);

    last_.set_bounds(kLapLast);
    last_.configure("LAST --:--.--", color::kDim, 2, Align::Center);

    best_.set_bounds(kLapBest);
    best_.configure("BEST --:--.--", color::kAccent, 2, Align::Center);

    lap_count_.set_bounds(kLapCount);
    lap_count_.configure("LAPS", "", 0, 3);

    speed_.set_bounds(kLapSpeed);
    speed_.configure("SPEED", "mph", 0, 3);

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

    char line[32];
    size_t k = 0;
    const char* tag = "LAST ";
    for (const char* p = tag; *p; ++p) line[k++] = *p;
    k += u::fmt_duration(line + k, sizeof line - k, f.lap.last_ms, f.lap.count > 0);
    line[k] = '\0';
    last_.set_text(line);

    k = 0;
    tag = "BEST ";
    for (const char* p = tag; *p; ++p) line[k++] = *p;
    k += u::fmt_duration(line + k, sizeof line - k, f.lap.best_ms, f.lap.best_ms > 0);
    line[k] = '\0';
    best_.set_text(line);

    // No reference trace yet, so there is no honest delta to draw. The
    // widget shows `--` rather than a zero, which would read as "dead on
    // your best lap" and be a lie (§18: the reference is still an open
    // question).
    delta_.set(f.lap.delta_ms, f.lap.best_ms > 0 && f.lap.delta_ms != 0);

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

WidgetList LapScreen::widgets() { return WidgetList{list_, 7}; }

// --- LapHistoryScreen ---------------------------------------------------
namespace {
constexpr Rect kHistTitle { 16,  24, 288,  16};
constexpr Rect kHistTable { 16,  56, 288, 380};
}  // namespace

void LapHistoryScreen::enter(Canvas&) {
    title_.set_bounds(kHistTitle);
    title_.configure("LAP      TIME        DELTA", color::kDim, 1, Align::Left);
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

WidgetList LapHistoryScreen::widgets() { return WidgetList{list_, 2}; }

// --- TrackMapScreen -----------------------------------------------------
namespace {
constexpr Rect kTrkMap  { 10,  40, 300, 300};
constexpr Rect kTrkLap  {  0, 350, 320,  40};
constexpr Rect kTrkLine { 16, 400, 288,  60};
}  // namespace

void TrackMapScreen::enter(Canvas&) {
    map_.set_bounds(kTrkMap);
    lap_.set_bounds(kTrkLap);
    lap_.configure("--:--.--", color::kText, 3, Align::Center);
    to_line_.set_bounds(kTrkLine);
    to_line_.configure("TO LINE", "ft", 0, 3);
    list_[0] = &map_;
    list_[1] = &lap_;
    list_[2] = &to_line_;
}

void TrackMapScreen::update(const Frame& f) {
    char buf[16];
    u::fmt_duration(buf, sizeof buf, f.lap.current_ms,
                    f.lap.phase == core::lap::Phase::Timing);
    lap_.set_text(buf);

    to_line_.set(f.lap.dist_to_line_ft,
                 f.gnss.fix && f.lap.phase != core::lap::Phase::NoLine);
    map_.set_position(0.0f, 0.0f, f.gnss.course_deg, f.gnss.fix);
}

WidgetList TrackMapScreen::widgets() { return WidgetList{list_, 3}; }

}  // namespace ui
