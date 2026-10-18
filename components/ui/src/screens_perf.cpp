// STAT -> PERF and STUNT, and CRUISE -> PHONE: the performance timers, the
// wheelie score, and the phone's last call or message.
#include "ui/screens.hpp"
#include "core/stunt/stunt.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"

#include <cstdio>
#include <cstring>

namespace ui {

using core::state::Frame;
namespace u = core::units;
using P = core::state::Perf;

namespace {
bool metric_units(core::units::System s) { return s == core::units::System::Metric; }

void fmt_s(char* buf, size_t n, uint32_t ms, int places) {
    const size_t k = u::fmt_float(buf, n, ms / 1000.0f, places);
    if (k + 1 < n) { buf[k] = 's'; buf[k + 1] = '\0'; }
}
}  // namespace

const char* perf_metric_name(uint8_t m, bool metric) {
    switch (m) {
        case P::To1:    return metric ? "0-50 KM/H" : "0-30 MPH";
        case P::To2:    return metric ? "0-100 KM/H" : "0-60 MPH";
        case P::Ft60:   return "60 FT";
        case P::Ft330:  return "330 FT";
        case P::Eighth: return "1/8 MILE";
        case P::Brake1: return metric ? "50-0 KM/H" : "30-0 MPH";
        case P::Brake2: return metric ? "100-0 KM/H" : "60-0 MPH";
        default:        return "";
    }
}

// --- PerfScreen ---------------------------------------------------------
namespace {
constexpr Rect kPerfStatus{14, 34, 452, 16};
constexpr int16_t kPerfTop = 82, kPerfRow = 28;
constexpr int16_t kPerfNameX = 14, kPerfLastX = 176, kPerfBestX = 324;
constexpr int16_t kPerfColW = 142;

// One result as the table shows it: a time, a time and its trap speed, or
// a stopping distance.
void fmt_result(char* buf, size_t n, int m, const P::Result& r, bool metric) {
    if (r.ms == 0) {
        std::snprintf(buf, n, "--");
        return;
    }
    char t[12], x[12];
    fmt_s(t, sizeof t, r.ms, 2);
    if (m == P::Brake1 || m == P::Brake2) {
        u::fmt_float(x, sizeof x, metric ? r.x * 0.3048f : r.x, 0);
        std::snprintf(buf, n, "%s %s", x, metric ? "m" : "ft");
    } else if (m >= P::Ft60) {
        u::fmt_float(x, sizeof x, metric ? r.x * 1.609344f : r.x, 0);
        std::snprintf(buf, n, "%s @%s", t, x);
    } else {
        std::snprintf(buf, n, "%s", t);
    }
}
}  // namespace

void PerfScreen::enter(Canvas&) {
    status_.set_bounds(kPerfStatus);
    status_.configure("", theme::secondary, 2, Align::Left);
    list_[0] = &status_;
    for (int i = 0; i < kRows; ++i) {
        const int16_t y = static_cast<int16_t>(kPerfTop + i * kPerfRow);
        name_[i].set_bounds(Rect{kPerfNameX, y, 158, 16});
        name_[i].configure("", theme::secondary, 2, Align::Left);
        last_[i].set_bounds(Rect{kPerfLastX, y, kPerfColW, 16});
        last_[i].configure("--", color::kDim, 2, Align::Right);
        best_[i].set_bounds(Rect{kPerfBestX, y, kPerfColW, 16});
        best_[i].configure("--", color::kDim, 2, Align::Right);
        list_[1 + i] = &name_[i];
        list_[1 + kRows + i] = &last_[i];
        list_[1 + 2 * kRows + i] = &best_[i];
    }
}

void PerfScreen::update(const Frame& f) {
    const P& p = f.perf;
    const bool metric = metric_units(units());
    char buf[40], t[12], d[12];
    switch (p.phase) {
        case 1:
            status_.configure("ARMED - GO WHEN READY", theme::nominal, 2, Align::Left);
            break;
        case 2:
        case 3:
            fmt_s(t, sizeof t, p.live_ms, 2);
            u::fmt_float(d, sizeof d, metric ? p.live_ft * 0.3048f : p.live_ft, 0);
            std::snprintf(buf, sizeof buf, "%s %s  %s %s", p.phase == 2 ? "LAUNCH" : "BRAKING", t, d,
                          metric ? "m" : "ft");
            status_.configure(buf, color::kAccent, 2, Align::Left);
            break;
        default:
            status_.configure("STOP FOR 1 S TO ARM", theme::secondary, 2, Align::Left);
            break;
    }
    for (int i = 0; i < kRows; ++i) {
        name_[i].set_text(perf_metric_name(static_cast<uint8_t>(i), metric));
        fmt_result(buf, sizeof buf, i, p.last[i], metric);
        // The newest result in the toast's colour, so the eye finds it.
        const bool newest = p.seq != 0 && p.event == i;
        last_[i].configure(buf, p.last[i].ms ? (newest ? color::kAccent : theme::primary) : color::kDim,
                           2, Align::Right);
        fmt_result(buf, sizeof buf, i, p.best[i], metric);
        best_[i].configure(buf, p.best[i].ms ? color::kWarn : color::kDim, 2, Align::Right);
    }
}

void PerfScreen::draw_background(Canvas& c) {
    const int16_t y = static_cast<int16_t>(kPerfTop - 22);
    c.draw_text(static_cast<int16_t>(kPerfLastX + kPerfColW), y, "LAST", theme::secondary, 1,
                Align::Right);
    c.draw_text(static_cast<int16_t>(kPerfBestX + kPerfColW), y, "BEST", theme::secondary, 1,
                Align::Right);
    c.fill_rect(Rect{14, static_cast<int16_t>(kPerfTop - 10), 452, 1}, theme::rule);
    // Launches above the rule, stops below it.
    c.fill_rect(Rect{14, static_cast<int16_t>(kPerfTop + 5 * kPerfRow - 7), 452, 1}, theme::rule);
    c.draw_text(14, 308, "Stop, then go: timed from the first movement.  E: clear bests",
                theme::secondary, 1, Align::Left);
}

WidgetList PerfScreen::widgets() { return WidgetList{list_, 1 + 3 * kRows}; }

bool PerfScreen::handle(const core::nav::InputEvent& e) {
    if (e.button != core::nav::Button::E || e.press != core::nav::Press::Short) return false;
    return post(core::state::Command(core::state::CommandId::PerfReset));
}

// --- StuntScreen --------------------------------------------------------
namespace {
constexpr Rect kStHead{14, 34, 300, 21};
constexpr Rect kStTime{14, 62, 300, 42};
constexpr Rect kStDetail{14, 112, 452, 14};
constexpr Rect kStCombo{320, 34, 146, 21};
constexpr Rect kStScore{320, 66, 146, 28};
constexpr Rect kStAngle{14, 136, 452, 14};
constexpr float kStAngleMax = 70.0f;
constexpr int16_t kStTileY[2] = {170, 236};
constexpr int16_t kStTileX[3] = {14, 168, 322};
constexpr int16_t kStTileW = 144, kStTileH = 58;
constexpr const char* kStCaption[6] = {
    "TRICKS", "BEST TRICK", "BEST COMBO",
    "LONGEST", "HIGHEST", "LAST LANDING",
};
Rect st_tile(int i) { return Rect{kStTileX[i % 3], kStTileY[i / 3], kStTileW, kStTileH}; }
int16_t angle_x(float deg) {
    return static_cast<int16_t>(kStAngle.x + kStAngle.w * deg / kStAngleMax);
}
}  // namespace

void StuntScreen::enter(Canvas&) {
    head_.set_bounds(kStHead);
    head_.configure("NO TRICKS YET", theme::secondary, 3, Align::Left);
    time_.set_bounds(kStTime);
    time_.configure("--", color::kDim, 6, Align::Left);
    detail_.set_bounds(kStDetail);
    detail_.configure("", theme::secondary, 2, Align::Left);
    combo_.set_bounds(kStCombo);
    combo_.configure("", color::kAccent, 3, Align::Right);
    score_.set_bounds(kStScore);
    score_.configure("0", theme::primary, 4, Align::Right);
    angle_.set_bounds(kStAngle);
    angle_.configure(color::kAccent, color::kAlarm, 60.0f / kStAngleMax);
    list_[0] = &head_;
    list_[1] = &time_;
    list_[2] = &detail_;
    list_[3] = &combo_;
    list_[4] = &score_;
    list_[5] = &angle_;
    for (int i = 0; i < kTiles; ++i) {
        const Rect t = st_tile(i);
        value_[i].set_bounds(Rect{static_cast<int16_t>(t.x + 4), static_cast<int16_t>(t.y + 26),
                                  static_cast<int16_t>(t.w - 8), 21});
        value_[i].configure("--", color::kDim, 3, Align::Center);
        list_[6 + i] = &value_[i];
    }
}

void StuntScreen::update(const Frame& f) {
    namespace st = core::stunt;
    const core::state::Stunt& s = f.stunt;
    char buf[48], t[12];

    if (s.live_kind != st::None) {
        head_.configure(s.live_kind == st::Stoppie ? "STOPPIE" : "WHEELIE",
                        s.live_balance ? color::kWarn : color::kAccent, 3, Align::Left);
        fmt_s(t, sizeof t, s.live_ms, 1);
        time_.configure(t, theme::primary, 6, Align::Left);
        std::snprintf(buf, sizeof buf, "%d deg  +%u%s",
                      static_cast<int>((s.live_kind == st::Stoppie ? -s.live_deg : s.live_deg) + 0.5f),
                      static_cast<unsigned>(s.live_points * s.combo_x10 / 10.0f),
                      s.live_balance ? "  BALANCE POINT" : "");
        detail_.configure(buf, theme::primary, 2, Align::Left);
    } else if (s.seq != 0) {
        std::snprintf(buf, sizeof buf, "%s", st::grade_name(s.kind, s.grade));
        head_.configure(buf, s.kind == st::Looped ? color::kAlarm
                             : s.record ? color::kWarn : color::kGood, 3, Align::Left);
        std::snprintf(buf, sizeof buf, "+%u", static_cast<unsigned>(s.points));
        time_.configure(buf, theme::primary, 6, Align::Left);
        fmt_s(t, sizeof t, s.ms, 1);
        std::snprintf(buf, sizeof buf, "%s  %d deg  x%u.%u%s", t, static_cast<int>(s.peak_deg + 0.5f),
                      s.mult_x10 / 10u, s.mult_x10 % 10u, s.record ? "  RECORD" : "");
        detail_.configure(buf, theme::secondary, 2, Align::Left);
    } else {
        head_.configure("NO TRICKS YET", theme::secondary, 3, Align::Left);
        time_.configure("--", color::kDim, 6, Align::Left);
        detail_.configure("Lift the front: points while it's up", theme::secondary, 2, Align::Left);
    }

    // The bar is the nose up against the ground, live, whatever is going on.
    const float lift = s.live_deg > 0.0f ? s.live_deg : 0.0f;
    angle_.set(lift / kStAngleMax, true);

    if (s.combo > 0) {
        std::snprintf(buf, sizeof buf, "COMBO x%u.%u", s.combo_x10 / 10u, s.combo_x10 % 10u);
        combo_.configure(buf, color::kAccent, 2, Align::Right);
    } else {
        combo_.configure("", color::kAccent, 2, Align::Right);
    }
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(s.total));
    score_.configure(buf, s.total ? theme::primary : color::kDim, 4, Align::Right);

    const auto tile = [&](int i, const char* text, bool valid) {
        const uint8_t scale = text_width_px(text, 3) <= value_[i].bounds.w ? 3 : 2;
        value_[i].configure(text, valid ? theme::primary : color::kDim, scale, Align::Center);
    };
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(s.tricks));
    tile(0, buf, s.tricks > 0);
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(s.best_points));
    tile(1, s.best_points ? buf : "--", s.best_points > 0);
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(s.best_combo));
    tile(2, s.best_combo ? buf : "--", s.best_combo > 0);
    fmt_s(t, sizeof t, s.longest_ms, 1);
    tile(3, s.longest_ms ? t : "--", s.longest_ms > 0);
    std::snprintf(buf, sizeof buf, "%d deg", static_cast<int>(s.highest_deg + 0.5f));
    tile(4, s.highest_deg > 0.0f ? buf : "--", s.highest_deg > 0.0f);
    const char* land = s.seq ? st::landing_name(s.landing) : "";
    tile(5, land[0] ? land : (s.seq && s.kind != st::Looped ? "PLAIN" : "--"), land[0] != '\0');
}

void StuntScreen::draw_background(Canvas& c) {
    c.draw_text(static_cast<int16_t>(kStScore.right()), static_cast<int16_t>(kStScore.bottom() + 4),
                "SESSION SCORE", theme::secondary, 1, Align::Right);
    // The balance-point band under the angle bar, and its scale.
    const int16_t lo = angle_x(30.0f), hi = angle_x(50.0f);
    c.fill_rect(Rect{lo, static_cast<int16_t>(kStAngle.bottom() + 2), static_cast<int16_t>(hi - lo), 3},
                color::kWarn);
    for (int d = 0; d <= 70; d += 10) {
        c.draw_text(angle_x(static_cast<float>(d)), static_cast<int16_t>(kStAngle.bottom() + 7),
                    d == 0 ? "0" : d == 70 ? "70" : "", theme::secondary, 1, Align::Center);
    }
    c.draw_text(static_cast<int16_t>((lo + hi) / 2), static_cast<int16_t>(kStAngle.bottom() + 7),
                "BALANCE", color::kWarn, 1, Align::Center);
    for (int i = 0; i < kTiles; ++i) {
        const Rect t = st_tile(i);
        c.draw_round_rect(t, chrome::kRadius, theme::rule);
        c.draw_text(static_cast<int16_t>(t.x + 8), static_cast<int16_t>(t.y + 8), kStCaption[i],
                    theme::secondary, 1, Align::Left);
    }
    c.draw_text(14, 308, "Points while it's up; 30-50 deg is worth 1.5x.  E: new session",
                theme::secondary, 1, Align::Left);
}

WidgetList StuntScreen::widgets() { return WidgetList{list_, 6 + kTiles}; }

bool StuntScreen::handle(const core::nav::InputEvent& e) {
    if (e.button != core::nav::Button::E || e.press != core::nav::Press::Short) return false;
    return post(core::state::Command(core::state::CommandId::StuntReset));
}

// --- PhoneScreen --------------------------------------------------------
namespace {
constexpr Rect kPhKind{14, 40, 452, 14};
constexpr Rect kPhWho{14, 64, 452, 28};
constexpr int16_t kPhLineY = 112, kPhLineH = 26;
constexpr int kPhLineChars = 37;   // at scale 2, in 452 px
constexpr Rect kPhAge{14, 200, 452, 14};
constexpr Rect kPhLink{14, 296, 452, 12};

// The next line of `s` that fits `max` characters, broken at a space where
// there is one. Returns where the following line starts.
const char* wrap(const char* s, char* out, int max) {
    while (*s == ' ') ++s;
    const int len = static_cast<int>(std::strlen(s));
    int n = len < max ? len : max;
    if (n < len) {
        int k = n;
        while (k > 0 && s[k] != ' ') --k;
        if (k > 0) n = k;
    }
    // Never end inside a UTF-8 sequence.
    while (n > 0 && n < len && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    std::memcpy(out, s, static_cast<size_t>(n));
    out[n] = '\0';
    return s + n;
}
}  // namespace

void PhoneScreen::enter(Canvas&) {
    kind_.set_bounds(kPhKind);
    kind_.configure("", theme::secondary, 2, Align::Left);
    who_.set_bounds(kPhWho);
    who_.configure("", theme::primary, 4, Align::Left);
    for (int i = 0; i < kLines; ++i) {
        line_[i].set_bounds(Rect{14, static_cast<int16_t>(kPhLineY + i * kPhLineH), 452, 14});
        line_[i].configure("", theme::primary, 2, Align::Left);
    }
    age_.set_bounds(kPhAge);
    age_.configure("", theme::secondary, 2, Align::Left);
    link_.set_bounds(kPhLink);
    link_.configure("", theme::secondary, 1, Align::Left);
    list_[0] = &kind_;
    list_[1] = &who_;
    list_[2] = &age_;
    list_[3] = &link_;
    for (int i = 0; i < kLines; ++i) list_[4 + i] = &line_[i];
}

void PhoneScreen::update(const Frame& f) {
    const core::state::Notify& n = f.notify;
    const bool linked = f.health.ok(core::state::Dev::Phone);
    link_.set_text(linked ? "Phone connected" : "No phone. CONFIG -> Phone to pair one.");
    char buf[48];

    if (n.seq == 0) {
        kind_.configure("NOTHING YET", theme::secondary, 2, Align::Left);
        who_.set_text("");
        for (auto& l : line_) l.set_text("");
        age_.set_text("");
        return;
    }
    const bool ringing = n.kind == 'C' && n.ringing && f.t_ms - n.at_ms < 45000;
    const char* kind = n.kind == 'C' ? (ringing ? "INCOMING CALL" : "CALL")
                     : n.kind == 'X' ? "MISSED CALL"
                     : n.kind == 'M' ? "MESSAGE" : "NOTIFICATION";
    kind_.configure(kind, ringing ? theme::nominal : n.kind == 'X' ? theme::warning : color::kAccent,
                    2, Align::Left);
    // The name at 4 if it fits, 3 if not; the font cuts nothing itself.
    const uint8_t scale = text_width_px(n.who, 4) <= kPhWho.w ? 4 : 3;
    who_.configure(n.who, theme::primary, scale, Align::Left);

    const bool moving = f.motion.best_mph >= 3.0f;
    const bool show = !moving || (has_ctx() && ctx().settings && ctx().settings->notify_text_moving);
    if (!n.text[0]) {
        for (auto& l : line_) l.set_text("");
    } else if (!show) {
        line_[0].configure("Stop to read it.", theme::secondary, 2, Align::Left);
        line_[1].set_text("");
        line_[2].set_text("");
    } else {
        const char* rest = n.text;
        for (int i = 0; i < kLines; ++i) {
            char part[kPhLineChars * 4 + 1];
            rest = wrap(rest, part, kPhLineChars);
            line_[i].configure(part, theme::primary, 2, Align::Left);
        }
    }

    const uint32_t age_s = (f.t_ms - n.at_ms) / 1000u;
    if (age_s < 60) std::snprintf(buf, sizeof buf, "just now");
    else if (age_s < 3600) std::snprintf(buf, sizeof buf, "%u min ago", static_cast<unsigned>(age_s / 60));
    else std::snprintf(buf, sizeof buf, "%u h ago", static_cast<unsigned>(age_s / 3600));
    age_.set_text(buf);
}

WidgetList PhoneScreen::widgets() { return WidgetList{list_, 4 + kLines}; }

}  // namespace ui
