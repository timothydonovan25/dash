#include "ui/screens.hpp"
#include "core/gearcal/gearcal.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"

namespace ui {

using core::gearcal::Phase;
using core::gearcal::Reason;
using core::state::Frame;
namespace u = core::units;

// The rider is on the bike when this is up, so the one thing they need
// (what to do next) is five scale-units tall and the numbers are secondary.
namespace {
constexpr Rect kHead   {14,  32, 452, 24};
constexpr Rect kStatus {14,  60, 452, 40};
constexpr Rect kDetail {14, 104, 452, 16};
constexpr Rect kBar    {14, 126, 452, 12};
constexpr Rect kLine   {14, 144, 452, 16};
constexpr Rect kTable  {14, 166, 452, 140};
constexpr Rect kHint   {14, 306, 452, 10};

const char* const kOrdinal[] = {"1st", "2nd", "3rd", "4th", "5th", "6th"};

// Append `s` at `k`, within `n`.
size_t put(char* buf, size_t n, size_t k, const char* s) {
    while (*s && k + 1 < n) buf[k++] = *s++;
    buf[k] = '\0';
    return k;
}

size_t put_num(char* buf, size_t n, size_t k, float v, int decimals) {
    if (k + 1 >= n) return k;
    return k + u::fmt_float(buf + k, n - k, v, decimals);
}

// "+1.2%" — signed, because which way it is off is the whole message.
size_t put_pct(char* buf, size_t n, size_t k, float pct) {
    k = put(buf, n, k, pct < 0.0f ? "-" : "+");
    k = put_num(buf, n, k, pct < 0.0f ? -pct : pct, 1);
    return put(buf, n, k, "%");
}

}  // namespace

void GearCalScreen::enter(Canvas&) {
    head_.set_bounds(kHead);     head_.configure("", color::kText, 3, Align::Left);
    status_.set_bounds(kStatus); status_.configure("", color::kText, 5, Align::Left);
    detail_.set_bounds(kDetail); detail_.configure("", color::kDim, 2, Align::Left);
    bar_.set_bounds(kBar);       bar_.configure(color::kAccent, color::kGood, 1.0f);
    line_.set_bounds(kLine);     line_.configure("", color::kText, 2, Align::Left);
    table_.set_bounds(kTable);   table_.configure(7, 2);
    hint_.set_bounds(kHint);     hint_.configure("", color::kDim, 1, Align::Left);
    list_[0] = &head_;  list_[1] = &status_; list_[2] = &detail_;
    list_[3] = &bar_;   list_[4] = &line_;   list_[5] = &table_;
    list_[6] = &hint_;
}

void GearCalScreen::update(const Frame& f) {
    const core::state::GearCal& c = f.gear_cal;
    phase_ = c.phase;
    const core::config::Settings* st = has_ctx() ? ctx().settings : nullptr;
    const uint8_t n = c.count > core::gearcal::kMaxGears ? core::gearcal::kMaxGears : c.count;

    char b[48];

    if (c.phase == static_cast<uint8_t>(Phase::Intro)) {
        head_.configure("CALIBRATE GEARS", color::kText, 3, Align::Left);
        status_.configure("RIDE EACH GEAR STEADY", color::kText, 3, Align::Left);
        detail_.set_text("10 s steady per gear, GPS fix");
        bar_.set_hidden(true);
        line_.set_text(c.autotune ? "Learned error (auto-adjust on)"
                                  : "Learned error (auto-adjust off)");
        for (uint8_t g = 0; g < n; ++g) {
            size_t k = put(b, sizeof b, 0, kOrdinal[g]);
            k = put(b, sizeof b, k, " ");
            if (st) k = put_num(b, sizeof b, k, st->gear_rpm_per_mph[g], 1);
            put(b, sizeof b, k, " r/mph");
            char v[16];
            if (c.learn_n[g] == 0) {
                put(v, sizeof v, 0, "--");
            } else {
                size_t j = put_pct(v, sizeof v, 0, c.learn_err_pct[g]);
                j = put(v, sizeof v, j, " n");
                put_num(v, sizeof v, j, c.learn_n[g], 0);
            }
            table_.set_row(g, b, v);
        }
        table_.set_row_count(n);
        hint_.set_text("E start   B back");
        return;
    }

    if (c.phase == static_cast<uint8_t>(Phase::Summary)) {
        head_.configure("RESULTS", color::kText, 3, Align::Left);
        status_.configure("E APPLY   B DISCARD", color::kText, 3, Align::Left);
        bar_.set_hidden(true);
        line_.set_text("measured vs table");
        if (c.circ_valid) {
            // GPS over the wheel: positive means the wheel reads low, and
            // the circumference it was set with is too small.
            size_t k = put(b, sizeof b, 0, "Wheel reads ");
            k = put_num(b, sizeof b, k, c.circ_err_pct < 0.0f ? -c.circ_err_pct : c.circ_err_pct, 1);
            put(b, sizeof b, k, c.circ_err_pct > 0.0f ? "% low vs GPS" : "% high vs GPS");
            detail_.configure(b, theme::warning, 2, Align::Left);
        } else {
            detail_.configure("Wheel speed matches GPS", theme::nominal, 2, Align::Left);
        }
        size_t rows = 0;
        for (uint8_t g = 0; g < n; ++g) {
            size_t k = put(b, sizeof b, 0, kOrdinal[g]);
            k = put(b, sizeof b, k, " ");
            char v[16];
            if (c.done_mask & (1u << g)) {
                k = put_num(b, sizeof b, k, c.result_ratio[g], 1);
                put(b, sizeof b, k, " r/mph");
                put_pct(v, sizeof v, 0, c.result_err_pct[g]);
            } else {
                put(b, sizeof b, k, "unchanged");
                put(v, sizeof v, 0, "skipped");
            }
            table_.set_row(rows++, b, v);
        }
        table_.set_row_count(rows);
        hint_.set_text(c.circ_valid ? "check Rear/Front circ. in CONFIG -> Wheels first"
                                    : "accepted values also become the learner's base");
        return;
    }

    // Measuring, one gear.
    const uint8_t g = c.gear;
    char head[32];
    size_t k = put(head, sizeof head, 0, "GEAR ");
    k = put(head, sizeof head, k, g >= 1 && g <= 6 ? kOrdinal[g - 1] : "?");
    k = put(head, sizeof head, k, "  ");
    k = put_num(head, sizeof head, k, g, 0);
    k = put(head, sizeof head, k, " of ");
    put_num(head, sizeof head, k, c.count, 0);
    head_.configure(head, color::kText, 3, Align::Left);

    const Reason r = static_cast<Reason>(c.reason);
    const bool last = g >= c.count;
    const char* word = "HOLD STEADY";
    Color wc = theme::warning;
    if (c.ready) {
        word = last ? "OK - PRESS E" : "OK - SHIFT UP";
        wc = theme::nominal;
    } else {
        switch (r) {
            case Reason::Neutral:   word = "IN NEUTRAL";  wc = theme::alarm; break;
            case Reason::Revs:      word = "MORE REVS";   break;
            case Reason::Slow:      word = "FASTER";      break;
            case Reason::NoGps:     word = "NO GPS FIX";  wc = theme::alarm; break;
            case Reason::GpsApart:  word = "WHEEL/GPS";   wc = theme::alarm; break;
            case Reason::WrongGear: word = "WRONG GEAR?"; wc = theme::alarm; break;
            case Reason::Steady:
            case Reason::Unsteady:  break;
        }
    }
    status_.configure(word, wc, 5, Align::Left);

    if (c.ready) {
        detail_.configure(last ? "Done. Hold longer to refine." : "Hold longer to refine, or shift up",
                          color::kDim, 2, Align::Left);
    } else if (r == Reason::WrongGear && c.nearest >= 1 && c.nearest <= 6) {
        size_t j = put(b, sizeof b, 0, "Looks like ");
        put(b, sizeof b, j, kOrdinal[c.nearest - 1]);
        detail_.configure(b, color::kDim, 2, Align::Left);
    } else {
        detail_.configure(core::gearcal::reason_text(r), color::kDim, 2, Align::Left);
    }

    bar_.set_hidden(false);
    bar_.set(c.need_ds ? static_cast<float>(c.steady_ds) / c.need_ds : 0.0f, true);

    k = put_num(b, sizeof b, 0, c.steady_ds / 10.0f, 1);
    k = put(b, sizeof b, k, "/");
    k = put_num(b, sizeof b, k, c.need_ds / 10.0f, 0);
    k = put(b, sizeof b, k, "s ");
    if (c.live_ratio > 0.0f) {
        k = put_num(b, sizeof b, k, c.live_ratio, 1);
        k = put(b, sizeof b, k, " ");
        put_pct(b, sizeof b, k, c.live_err_pct);
    }
    line_.set_text(b);

    size_t rows = 0;
    for (uint8_t i = 0; i < g - 1 && i < core::gearcal::kMaxGears; ++i) {
        size_t j = put(b, sizeof b, 0, kOrdinal[i]);
        j = put(b, sizeof b, j, " ");
        char v[16];
        if (c.done_mask & (1u << i)) {
            j = put_num(b, sizeof b, j, c.result_ratio[i], 1);
            put(b, sizeof b, j, " r/mph");
            put_pct(v, sizeof v, 0, c.result_err_pct[i]);
        } else {
            put(b, sizeof b, j, "skipped");
            put(v, sizeof v, 0, "--");
        }
        table_.set_row(rows++, b, v);
    }
    table_.set_row_count(rows);
    hint_.set_text("E next   hold E skip gear   B cancel");
}

bool GearCalScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    using core::state::CommandId;

    // Every button is this page's while it is up: a rider mid-run who
    // brushes A or D must not leave the page with the run still open.
    if (e.button == Button::E) {
        if (e.press == Press::Long) {
            if (phase_ == static_cast<uint8_t>(Phase::Measuring)) {
                post(core::state::Command(CommandId::GearCalSkip));
            }
        } else {
            post(core::state::Command(CommandId::GearCalNext));
        }
    } else if (e.button == Button::B) {
        post(core::state::Command(CommandId::GearCalCancel));
    }
    return true;
}

WidgetList GearCalScreen::widgets() { return WidgetList{list_, 7}; }

}  // namespace ui
