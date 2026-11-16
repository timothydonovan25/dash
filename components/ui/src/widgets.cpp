#include "ui/widgets.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {

namespace {

// A non-finite reading is no reading. NaN also compares unequal to itself,
// so letting one through would mark its widget dirty on every frame.
inline bool usable(float v, bool valid) { return valid && std::isfinite(v); }

// Lean, as the gauges hold it: half-degree steps, and a step taken only
// once the angle has moved a whole degree from what is shown. IMU noise
// dithering across a half-degree boundary otherwise repaints a 31k px arc
// on almost every frame of a ride.
constexpr float kLeanHoldDeg = 1.0f;
inline float held_lean(float shown, bool was_valid, float lean_deg) {
    const float q = std::round(lean_deg * 2.0f) / 2.0f;
    return was_valid && std::fabs(q - shown) < kLeanHoldDeg ? shown : q;
}
constexpr float kPi = 3.14159265358979f;

// The characters that differ between two strings drawn the same way, as one
// rectangle in screen coordinates (empty when none do). False when the
// lengths differ: then a centred or right-aligned string has moved as a
// whole, and the caller sends its bounds. Mirrors draw_text's placement.
bool changed_cells(const char* a, const char* b, int16_t x, int16_t y,
                   uint8_t scale, Align align, Rect& out) {
    const size_t n = std::strlen(a);
    if (std::strlen(b) != n) return false;
    if (scale == 0) scale = 1;
    const Font& f = font_5x7();
    const int16_t w = text_width_px(a, scale);
    int16_t pen = x;
    if (align == Align::Center) pen = static_cast<int16_t>(x - w / 2);
    else if (align == Align::Right) pen = static_cast<int16_t>(x - w);
    size_t i0 = n, i1 = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            if (i0 == n) i0 = i;
            i1 = i;
        }
    }
    if (i0 == n) {
        out = Rect{};
        return true;
    }
    out = Rect{static_cast<int16_t>(pen + static_cast<int16_t>(i0) * f.advance * scale), y,
               static_cast<int16_t>((static_cast<int16_t>(i1 - i0) * f.advance + f.width) * scale),
               static_cast<int16_t>(f.height * scale)};
    return true;
}

// The smallest rectangle holding every point, grown by `margin`.
struct Box {
    int16_t x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;
    void add(int16_t x, int16_t y) {
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
    }
    Rect rect(int16_t margin) const {
        if (x1 < x0) return Rect{};
        return Rect{static_cast<int16_t>(x0 - margin), static_cast<int16_t>(y0 - margin),
                    static_cast<int16_t>(x1 - x0 + 1 + 2 * margin),
                    static_cast<int16_t>(y1 - y0 + 1 + 2 * margin)};
    }
};

// Copy with truncation. Returns whether anything changed, which is what
// drives the dirty flag.
bool copy_if_changed(char* dst, size_t cap, const char* src) {
    if (!src) src = "";
    size_t i = 0;
    bool changed = false;
    for (; src[i] && i + 1 < cap; ++i) {
        if (dst[i] != src[i]) { dst[i] = src[i]; changed = true; }
    }
    if (dst[i] != '\0') { changed = true; }
    dst[i] = '\0';
    return changed;
}
}  // namespace

template <size_t N>
bool TextBuf<N>::set(const char* v) { return copy_if_changed(s, N, v); }

// The instantiations the widgets below need.
template struct TextBuf<8>;
template struct TextBuf<12>;
template struct TextBuf<14>;
template struct TextBuf<16>;
template struct TextBuf<20>;
template struct TextBuf<24>;
template struct TextBuf<28>;
template struct TextBuf<32>;   // ui::ReadoutWidget
template struct TextBuf<60>;

// --- Label --------------------------------------------------------------

// configure() is idempotent on purpose. Some screens re-configure a widget
// every frame — a caption whose colour tracks a state, a unit that follows
// the metric setting — and a configure() that always marked itself dirty
// would repaint those widgets 30 times a second forever.
void Label::configure(const char* text, Color c, uint8_t scale, Align align) {
    const bool a = text_.set(text);
    const bool b = assign_t(color_, c);
    const bool d = assign_t(scale_, scale);
    const bool e = assign_t(align_, align);
    if (a || b || d || e) touch();
}

void Label::set_text(const char* text) {
    if (text_.set(text)) touch_part();
}

int Label::damage(Rect* out, int max) const {
    (void)max;
    if (!whole_) {
        int16_t x = bounds.x;
        if (align_ == Align::Center) x = bounds.cx();
        else if (align_ == Align::Right) x = bounds.right();
        Rect r;
        if (changed_cells(sent_.s, text_.s, x, bounds.y, scale_, align_, r)) {
            r = r.clipped_to(bounds);
            if (r.empty()) return 0;
            out[0] = r;
            return 1;
        }
    }
    out[0] = bounds;
    return 1;
}

void Label::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    int16_t x = bounds.x;
    if (align_ == Align::Center) x = bounds.cx();
    else if (align_ == Align::Right) x = bounds.right();
    c.draw_text(x, bounds.y, text_.s, color_, scale_, align_);
}

// --- ValueBox -----------------------------------------------------------

void ValueBox::configure(const char* label, const char* unit, int decimals,
                         uint8_t scale) {
    const bool a = label_.set(label);
    const bool b = unit_.set(unit);
    const bool d = assign_t(decimals_, decimals);
    const bool e = assign_t(scale_, scale);
    if (a || b || d || e) touch();
}

void ValueBox::set(float value, bool valid) {
    // Compare at display resolution. A battery voltage jittering in the
    // fourth decimal must not repaint the widget 30 times a second.
    float scale = 1.0f;
    for (int i = 0; i < decimals_; ++i) scale *= 10.0f;
    valid = usable(value, valid);
    const float q = valid ? std::round(value * scale) / scale : value_;
    const bool changed = assign_t(valid_, valid) | (valid && assign(value_, q));
    if (changed) touch();
}

void ValueBox::set_unit(const char* unit) { if (unit_.set(unit)) touch(); }

void ValueBox::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    c.draw_text(bounds.x, bounds.y, label_.s, color::kDim, 1, Align::Left);

    char buf[24];
    core::units::fmt_value(buf, sizeof buf, value_, decimals_, valid_);
    const int16_t y = static_cast<int16_t>(bounds.y + c.text_height(1) + 4);
    c.draw_text(bounds.x, y, buf, valid_ ? color::kText : color::kDim,
                scale_, Align::Left);

    if (!unit_.empty()) {
        const int16_t vx = static_cast<int16_t>(bounds.x + c.text_width(buf, scale_) + 6);
        const int16_t vy = static_cast<int16_t>(y + c.text_height(scale_) - c.text_height(1));
        c.draw_text(vx, vy, unit_.s, color::kDim, 1, Align::Left);
    }
}

// --- BigNumber ----------------------------------------------------------

void BigNumber::configure(const char* unit, int decimals, uint8_t scale) {
    const bool a = unit_.set(unit);
    const bool b = assign_t(decimals_, decimals);
    const bool d = assign_t(scale_, scale);
    if (a || b || d) touch();
}

void BigNumber::set(float value, bool valid) {
    float scale = 1.0f;
    for (int i = 0; i < decimals_; ++i) scale *= 10.0f;
    valid = usable(value, valid);
    const float q = valid ? std::round(value * scale) / scale : value_;
    // Valid or not changes the colour: everything. A new value only changes
    // digits, which damage() finds.
    if (valid_ != valid) {
        valid_ = valid;
        touch();
    }
    if (valid && value_ != q) {
        value_ = q;
        touch_part();
    }
}

int BigNumber::damage(Rect* out, int max) const {
    (void)max;
    if (!whole_ && sent_valid_ == valid_) {
        char was[16], now[16];
        core::units::fmt_value(was, sizeof was, sent_value_, decimals_, sent_valid_);
        core::units::fmt_value(now, sizeof now, value_, decimals_, valid_);
        const int16_t th = static_cast<int16_t>(font_5x7().height * scale_);
        const int16_t y = static_cast<int16_t>(bounds.cy() - th / 2);
        Rect r;
        if (changed_cells(was, now, bounds.cx(), y, scale_, Align::Center, r)) {
            r = r.clipped_to(bounds);
            if (r.empty()) return 0;
            out[0] = r;
            return 1;
        }
    }
    out[0] = bounds;
    return 1;
}

void BigNumber::set_color(Color c) { assign_t(color_, c); }

void BigNumber::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    char buf[16];
    core::units::fmt_value(buf, sizeof buf, value_, decimals_, valid_);

    const int16_t th = c.text_height(scale_);
    const int16_t y = static_cast<int16_t>(bounds.cy() - th / 2);
    c.draw_text(bounds.cx(), y, buf, valid_ ? color_ : color::kDim, scale_,
                Align::Center);

    if (!unit_.empty()) {
        c.draw_text(bounds.cx(), static_cast<int16_t>(y + th + 6), unit_.s,
                    color::kDim, 2, Align::Center);
    }
}

// --- Bar ----------------------------------------------------------------

void Bar::configure(Color fill, Color warn_fill, float warn_above) {
    const bool a = assign_t(fill_, fill);
    const bool b = assign_t(warn_, warn_fill);
    const bool d = assign(warn_above_, warn_above);
    if (a || b || d) touch();
}

void Bar::set(float fraction, bool valid) {
    valid = usable(fraction, valid);
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    // A pixel of bar is worth repainting for; a thousandth is not. A bar
    // showing "no reading" does not follow the dead value underneath it.
    const float q = std::round(fraction * 100.0f) / 100.0f;
    const bool changed = assign_t(valid_, valid) | (valid && assign(fraction_, q));
    if (changed) touch();
}

void Bar::set_hidden(bool hidden) {
    if (assign_t(hidden_, hidden)) touch();
}

void Bar::render(Canvas& c) {
    if (hidden_) {
        c.fill_rect(bounds, color::kBg);
        return;
    }
    c.fill_rect(bounds, color::kPanel);
    c.draw_rect(bounds, color::kGrid);
    if (!valid_) {
        c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - 3), "--",
                    color::kDim, 1, Align::Center);
        return;
    }
    const Rect inner = bounds.inset(2);
    const int16_t w = static_cast<int16_t>(inner.w * fraction_);
    if (w > 0) {
        const Color fill = fraction_ >= warn_above_ ? warn_ : fill_;
        c.fill_rect(Rect{inner.x, inner.y, w, inner.h}, fill);
    }
}

// --- StatusDot ----------------------------------------------------------

void StatusDot::configure(const char* name) { if (name_.set(name)) touch(); }

void StatusDot::set(core::state::DevState s) { assign_t(state_, s); }

void StatusDot::render(Canvas& c) {
    using core::state::DevState;
    c.fill_rect(bounds, color::kBg);

    Color dot = color::kDim;
    const char* what = "--";
    switch (state_) {
        case DevState::Absent:   dot = color::kGrid;  what = "absent";   break;
        case DevState::Init:     dot = color::kWarn;  what = "init";     break;
        case DevState::Ok:       dot = color::kGood;  what = "ok";       break;
        case DevState::Degraded: dot = color::kWarn;  what = "degraded"; break;
        case DevState::Fault:    dot = color::kAlarm; what = "FAULT";    break;
    }

    const int16_t r = 4;
    c.fill_circle(static_cast<int16_t>(bounds.x + r), bounds.cy(), r, dot);
    c.draw_text(static_cast<int16_t>(bounds.x + 2 * r + 6),
                static_cast<int16_t>(bounds.cy() - 3), name_.s,
                color::kText, 1, Align::Left);
    c.draw_text(bounds.right(), static_cast<int16_t>(bounds.cy() - 3), what,
                dot, 1, Align::Right);
}

// --- ShiftStrip ---------------------------------------------------------

void ShiftStrip::set(uint8_t mask, bool flash) {
    const bool a = assign_t(mask_, mask);
    const bool b = assign_t(flash_, flash);
    if (a || b) touch();
}

void ShiftStrip::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t n = 8;
    const int16_t gap = 4;
    const int16_t w = static_cast<int16_t>((bounds.w - gap * (n - 1)) / n);
    for (int16_t i = 0; i < n; ++i) {
        const bool lit = flash_ ? true : ((mask_ >> i) & 1u) != 0;
        // One colour for the whole strip, from theme::shift_lamp, because the
        // lamps on the board are all red: how many are lit is the reading, not
        // what colour they are.
        const Color c_on = theme::shift_lamp(i);
        const Rect r{static_cast<int16_t>(bounds.x + i * (w + gap)), bounds.y,
                     w, bounds.h};
        c.fill_rect(r, lit ? c_on : color::kPanel);
    }
}

// --- DeltaBar -----------------------------------------------------------

void DeltaBar::set(int32_t delta_ms, bool valid) {
    const bool b = assign_t(valid_, valid);
    const bool a = valid && assign_t(delta_ms_, delta_ms);
    if (a || b) touch();
}

void DeltaBar::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t mid = bounds.cx();

    char buf[16];
    core::units::fmt_delta(buf, sizeof buf, delta_ms_, valid_);
    if (!valid_) {
        // No centre rule when there is nothing on the scale. An empty axis
        // with a tick in it reads as an artifact rather than as a reading
        // of zero, and on the speed screen there is no caption beside it to
        // say otherwise.
        c.draw_text(mid, bounds.y, buf, color::kDim, 2, Align::Center);
        return;
    }

    c.draw_vline(mid, bounds.y, bounds.h, color::kGrid);

    // Full scale is two seconds either way. Past that the bar pins and the
    // number carries the rest — a rider four seconds down does not need a
    // longer bar to know it.
    const float span = 2000.0f;
    float f = static_cast<float>(delta_ms_) / span;
    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;

    const int16_t half = static_cast<int16_t>(bounds.w / 2 - 2);
    const int16_t len = static_cast<int16_t>(f * half);
    const Color col = delta_ms_ <= 0 ? color::kGood : color::kAlarm;
    const int16_t bar_h = static_cast<int16_t>(bounds.h / 2);
    if (len >= 0) {
        c.fill_rect(Rect{mid, bounds.y, len, bar_h}, col);
    } else {
        c.fill_rect(Rect{static_cast<int16_t>(mid + len), bounds.y,
                         static_cast<int16_t>(-len), bar_h}, col);
    }
    c.draw_text(mid, static_cast<int16_t>(bounds.y + bar_h + 2), buf, col, 2,
                Align::Center);
}

// --- LeanIndicator ------------------------------------------------------

void LeanIndicator::set(float lean_deg, bool valid) {
    valid = usable(lean_deg, valid);
    const bool was_valid = valid_;
    const bool b = assign_t(valid_, valid);
    bool a = false;
    if (valid) {
        a = assign(lean_deg_, held_lean(lean_deg_, was_valid, lean_deg));
    }
    if (a || b) touch();
}

void LeanIndicator::set_peaks(float left_deg, float right_deg) {
    // Records, so they take the half-degree step, not the hold.
    const float l = -std::round(left_deg * 2.0f) / 2.0f;
    const float r = std::round(right_deg * 2.0f) / 2.0f;
    const bool a = assign(peak_left_, l);
    const bool b = assign(peak_right_, r);
    if (a || b) touch();
}

void LeanIndicator::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t cx = bounds.cx();
    const int16_t cy = bounds.cy();
    const int16_t r = static_cast<int16_t>((bounds.w < bounds.h ? bounds.w : bounds.h) / 2 - 2);
    c.draw_circle(cx, cy, r, color::kGrid);

    if (!valid_) {
        c.draw_text(cx, static_cast<int16_t>(cy - 3), "--", color::kDim, 2,
                    Align::Center);
        return;
    }

    // A horizon line that stays level while the bike rotates under it, which
    // is what the rider's inner ear expects.
    const float a = -lean_deg_ * kPi / 180.0f;
    const float ca = std::cos(a), sa = std::sin(a);
    c.draw_line(static_cast<int16_t>(cx - r * ca), static_cast<int16_t>(cy - r * sa),
                static_cast<int16_t>(cx + r * ca), static_cast<int16_t>(cy + r * sa),
                color::kAccent);

    char buf[16];
    core::units::fmt_float(buf, sizeof buf, std::fabs(lean_deg_), 0);
    c.draw_text(cx, static_cast<int16_t>(cy + r / 3), buf, color::kText, 2,
                Align::Center);

    char peaks[24];
    char l[8], rgt[8];
    core::units::fmt_float(l, sizeof l, -peak_left_, 0);
    core::units::fmt_float(rgt, sizeof rgt, peak_right_, 0);
    size_t k = 0;
    for (const char* p = l; *p && k + 1 < sizeof peaks; ++p) peaks[k++] = *p;
    if (k + 3 < sizeof peaks) { peaks[k++] = ' '; peaks[k++] = '/'; peaks[k++] = ' '; }
    for (const char* p = rgt; *p && k + 1 < sizeof peaks; ++p) peaks[k++] = *p;
    peaks[k] = '\0';
    c.draw_text(cx, static_cast<int16_t>(bounds.bottom() - c.text_height(1)),
                peaks, color::kDim, 1, Align::Center);
}

// --- Sparkline ----------------------------------------------------------

void Sparkline::configure(float min, float max, Color c) {
    const bool a = assign(min_, min);
    const bool b = assign(max_, max);
    const bool d = assign_t(color_, c);
    if (a || b || d) touch();
}

void Sparkline::push(float v) {
    points_[head_] = v;
    head_ = (head_ + 1) % kPoints;
    if (count_ < kPoints) ++count_;
    touch();
}

void Sparkline::clear() {
    count_ = 0;
    head_ = 0;
    touch();
}

void Sparkline::render(Canvas& c) {
    c.fill_rect(bounds, color::kPanel);
    if (count_ < 2) return;

    const float span = (max_ - min_) != 0.0f ? (max_ - min_) : 1.0f;
    int16_t prev_x = 0, prev_y = 0;
    for (size_t i = 0; i < count_; ++i) {
        const size_t idx = (head_ + kPoints - count_ + i) % kPoints;
        float f = (points_[idx] - min_) / span;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const int16_t x = static_cast<int16_t>(
            bounds.x + (bounds.w - 1) * static_cast<int>(i) / static_cast<int>(count_ - 1));
        const int16_t y = static_cast<int16_t>(bounds.bottom() - 1 - f * (bounds.h - 2));
        if (i > 0) c.draw_line(prev_x, prev_y, x, y, color_);
        prev_x = x;
        prev_y = y;
    }
}

// --- HrGraph -------------------------------------------------------------

void HrGraph::set(uint16_t zone_max_bpm) {
    const bool a = assign_t(max_, zone_max_bpm);
    const bool b = src_ && assign_t(seq_, src_->seq());
    if (a || b) touch();
}

void HrGraph::render(Canvas& c) {
    using core::hr::History;
    c.fill_rect(bounds, color::kPanel);
    // A margin for the scale on the left and the time under the line.
    constexpr int16_t kLeft = 26, kBottom = 12, kTop = 4;
    const Rect plot{static_cast<int16_t>(bounds.x + kLeft), static_cast<int16_t>(bounds.y + kTop),
                    static_cast<int16_t>(bounds.w - kLeft - 4),
                    static_cast<int16_t>(bounds.h - kTop - kBottom)};
    const int16_t label_y = static_cast<int16_t>(plot.bottom() + 3);
    c.draw_text(plot.x, label_y, "10 min", color::kDim, 1, Align::Left);
    c.draw_text(plot.right(), label_y, "now", color::kDim, 1, Align::Right);
    // A tick every two minutes.
    for (int m = 1; m < 5; ++m) {
        const int16_t x = static_cast<int16_t>(plot.x + plot.w * m / 5);
        c.draw_vline(x, plot.bottom(), 3, color::kGrid);
    }

    uint8_t lo8 = 0, hi8 = 0;
    if (!src_ || !src_->range(lo8, hi8)) {
        c.draw_text(plot.cx(), static_cast<int16_t>(plot.cy() - 3), "no readings yet",
                    color::kDim, 1, Align::Center);
        return;
    }
    // Ten bpm either side, on tens, and never under 40 tall: a steady
    // 100 is a flat line in the middle, not a jagged one filling the box.
    int lo = (lo8 - 10) / 10 * 10;
    int hi = (hi8 + 19) / 10 * 10;
    if (hi - lo < 40) {
        const int mid = (hi + lo) / 2;
        lo = mid - 20;
        hi = mid + 20;
    }
    if (lo < 0) { hi -= lo; lo = 0; }
    const auto y_of = [&](int v) {
        return static_cast<int16_t>(plot.bottom() - 1 - (v - lo) * (plot.h - 1) / (hi - lo));
    };
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d", hi);
    c.draw_text(static_cast<int16_t>(bounds.x + 2), plot.y, buf, color::kDim, 1, Align::Left);
    std::snprintf(buf, sizeof buf, "%d", lo);
    c.draw_text(static_cast<int16_t>(bounds.x + 2), static_cast<int16_t>(plot.bottom() - 7), buf,
                color::kDim, 1, Align::Left);

    // The zone edges that fall inside the scale, dotted.
    if (max_ > 0) {
        for (int pct = 50; pct <= 90; pct += 10) {
            const int v = max_ * pct / 100;
            if (v <= lo || v >= hi) continue;
            const int16_t y = y_of(v);
            for (int16_t x = plot.x; x < plot.right(); x += 4) c.draw_hline(x, y, 2, color::kGrid);
        }
    }

    int16_t px = 0, py = 0;
    bool have = false;
    for (size_t i = 0; i < History::kSamples; ++i) {
        const uint8_t v = src_->at(i);
        if (v == 0) { have = false; continue; }
        const int16_t x = static_cast<int16_t>(plot.x + (plot.w - 1) * static_cast<int>(i) /
                                                         static_cast<int>(History::kSamples - 1));
        const int16_t y = y_of(v);
        const Color col = chrome::hr_zone_color(core::hr::zone(v, max_));
        if (have) {
            c.draw_line(px, py, x, y, col);
            c.draw_line(px, static_cast<int16_t>(py + 1), x, static_cast<int16_t>(y + 1), col);
        } else {
            c.fill_rect(Rect{x, y, 2, 2}, col);   // a lone sample still shows
        }
        px = x;
        py = y;
        have = true;
    }
}

// --- Table --------------------------------------------------------------

void Table::configure(uint8_t visible_rows, uint8_t scale) {
    const uint8_t v = visible_rows > kRows ? static_cast<uint8_t>(kRows) : visible_rows;
    const bool a = assign_t(visible_, v);
    const bool b = assign_t(scale_, scale);
    if (a || b) touch();
}

void Table::set_row(size_t i, const char* left, const char* right) {
    if (i >= kRows) return;
    const bool a = left_[i].set(left);
    const bool b = right_[i].set(right);
    // A row coming back into the count must be sent even if its text matches:
    // a shrink leaves the old text in the buffer but not on the glass.
    while (rows_ <= i) mark_row(rows_++);
    if (a || b) mark_row(i);
}

void Table::set_row_count(size_t n) {
    if (n > kRows) n = kRows;
    if (rows_ != n) { rows_ = n; touch(); }
}

void Table::clear_rows() {
    // Only if there is something to clear: the lap history calls this every
    // frame while there are no laps, and an unconditional touch() sent the
    // whole 452x256 table, 23 ms of wire, sixty times a second.
    bool any = rows_ != 0;
    for (size_t i = 0; i < kRows && !any; ++i) any = !left_[i].empty() || !right_[i].empty();
    if (!any) return;
    for (size_t i = 0; i < kRows; ++i) { left_[i].set(""); right_[i].set(""); }
    rows_ = 0;
    header_mask_ = 0;
    touch();
}

void Table::set_focus(int focus) {
    if (focus == focus_) return;
    // A move that scrolls the list moves every row; one that does not
    // changes only the two rows the highlight leaves and lands on.
    if (first_visible(focus) != first_visible(focus_)) {
        focus_ = focus;
        touch();
        return;
    }
    if (focus_ >= 0) mark_row(static_cast<size_t>(focus_));
    focus_ = focus;
    if (focus_ >= 0) mark_row(static_cast<size_t>(focus_));
}

void Table::set_header(size_t i, bool header) {
    if (i >= kRows) return;
    const uint16_t bit = static_cast<uint16_t>(1u << i);
    if (((header_mask_ & bit) != 0) == header) return;
    header_mask_ = static_cast<uint16_t>(header ? header_mask_ | bit : header_mask_ & ~bit);
    mark_row(i);
}

void Table::set_editing(bool editing) {
    if (editing == editing_) return;
    editing_ = editing;
    if (focus_ >= 0) mark_row(static_cast<size_t>(focus_));
}

void Table::mark_row(size_t r) {
    if (r < kRows) row_mask_ = static_cast<uint16_t>(row_mask_ | (1u << r));
    touch_part();
}

size_t Table::first_visible(int focus) const {
    // Scroll so the focused row stays on screen. Without this the config
    // list simply stops responding once the cursor walks past the bottom.
    if (focus >= 0 && static_cast<size_t>(focus) >= visible_) {
        return static_cast<size_t>(focus) - visible_ + 1;
    }
    return 0;
}

int16_t Table::line_px() const {
    return static_cast<int16_t>(font_5x7().height * scale_ + 6);
}

int Table::damage(Rect* out, int max) const {
    if (whole_) { out[0] = bounds; return 1; }
    const size_t first = first_visible(focus_);
    const int16_t line = line_px();
    // A row owns the band from 2 px above its text to the next row's, which
    // is where render() puts the focus highlight. Adjacent changed rows go
    // as one rectangle: a transfer's command overhead is worth ~25 px.
    int n = 0;
    size_t i = 0;
    while (i < visible_) {
        if (first + i >= kRows || !(row_mask_ & (1u << (first + i)))) { ++i; continue; }
        size_t j = i;
        while (j + 1 < visible_ && first + j + 1 < kRows &&
               (row_mask_ & (1u << (first + j + 1)))) {
            ++j;
        }
        if (n == max) { out[0] = bounds; return 1; }
        const Rect band{bounds.x, static_cast<int16_t>(bounds.y + i * line - 2), bounds.w,
                        static_cast<int16_t>((j - i + 1) * line)};
        const Rect r = band.clipped_to(bounds);
        if (!r.empty()) out[n++] = r;
        i = j + 1;
    }
    return n;
}

void Table::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t line = line_px();
    const size_t first = first_visible(focus_);

    for (size_t i = 0; i < visible_ && first + i < rows_; ++i) {
        const size_t r = first + i;
        const int16_t y = static_cast<int16_t>(bounds.y + i * line);
        if (header_mask_ & (1u << r)) {
            // Sits on the row's baseline, small, with a rule out to the edge.
            const int16_t ty = static_cast<int16_t>(y + line - 7 - 6);
            c.draw_text(static_cast<int16_t>(bounds.x + kPadPx), ty, left_[r].s,
                        theme::secondary, 1, Align::Left);
            const int16_t tx = static_cast<int16_t>(bounds.x + kPadPx + c.text_width(left_[r].s, 1) + 6);
            c.fill_rect(Rect{tx, static_cast<int16_t>(ty + 3),
                             static_cast<int16_t>(bounds.right() - kPadPx - tx), 1}, theme::rule);
            continue;
        }
        const bool focused = static_cast<int>(r) == focus_;
        if (focused) {
            // Inside the bounds: on the first row the band starts above
            // them, and a partial repaint never clears what is out there.
            // The outline in the text colour says which row it is where the
            // panel fill alone is too faint to see (a transflective panel).
            const Rect band = Rect{bounds.x, static_cast<int16_t>(y - 2), bounds.w,
                                   static_cast<int16_t>(line)}.clipped_to(bounds);
            c.fill_rect(band, color::kPanel);
            c.draw_rect(band, editing_ ? color::kAccent : theme::primary);
            if (editing_) c.draw_rect(Rect{static_cast<int16_t>(band.x + 1), static_cast<int16_t>(band.y + 1),
                                           static_cast<int16_t>(band.w - 2), static_cast<int16_t>(band.h - 2)},
                                      color::kAccent);
        }
        // Inset off the outline, every row alike so the columns line up.
        c.draw_text(static_cast<int16_t>(bounds.x + kPadPx), y, left_[r].s,
                    focused ? color::kWhite : color::kText, scale_, Align::Left);
        c.draw_text(static_cast<int16_t>(bounds.right() - kPadPx), y, right_[r].s,
                    focused ? color::kAccent : color::kDim, scale_, Align::Right);
    }
}

// --- MapView ------------------------------------------------------------

void MapView::set_source(const IPointSource* src) {
    src_ = src;
    fit_stale_ = true;
    touch();
}

void MapView::set_route(const IPointSource* route) {
    route_ = route;
    drawn_route_ = route ? route->total() : 0;
    fit_stale_ = true;
    touch();
}

void MapView::set_labels(const core::state::Nav::Label* l, uint8_t n) {
    if (n > core::state::Nav::kMaxLabels) n = core::state::Nav::kMaxLabels;
    bool same = n == n_labels_;
    for (uint8_t i = 0; same && i < n; ++i)
        same = l[i].x_ft == labels_[i].x_ft && l[i].y_ft == labels_[i].y_ft &&
               std::strcmp(l[i].name, labels_[i].name) == 0;
    if (same) return;
    for (uint8_t i = 0; i < n; ++i) labels_[i] = l[i];
    n_labels_ = n;
    touch();
}

void MapView::set_live(bool valid) {
    assign_t(valid_, valid);
    // The trace is redrawn as it grows, every 10 points (1 s at 10 Hz), or
    // at once when it is cleared for a new lap. Nothing else moves it: the
    // GPS course used to dirty it on every fix, 79k px (16 ms of wire) for
    // a heading the map never drew.
    const size_t t = src_ ? src_->total() : 0;
    if (t < drawn_total_ || t >= drawn_total_ + kRedrawPoints) {
        drawn_total_ = t;
        fit_stale_ = true;
        touch();
    }
    const size_t rt = route_ ? route_->total() : 0;
    if (rt != drawn_route_) {
        drawn_route_ = rt;
        fit_stale_ = true;
        touch();
    }
    if (fit_stale_) refit();
}

void MapView::refit() {
    fit_stale_ = false;
    fit_ok_ = false;
    fit_n_ = src_ ? src_->count() : 0;
    fit_route_n_ = route_ ? route_->count() : 0;
    if (fit_route_n_ < 2) fit_route_n_ = 0;
    if (fit_n_ < 2 && fit_route_n_ == 0) return;
    // Fit the whole trace and the route, walking them rather than keeping a
    // copy. The buffers own these points (§4.4) and this is a reader. The
    // route view fits the route alone, once there is one.
    const size_t from = route_only_ && fit_route_n_ ? fit_n_ : 0;
    for (size_t i = from; i < fit_n_ + fit_route_n_; ++i) {
        float x, y;
        const bool got = i < fit_n_ ? src_->point(i, x, y) : route_->point(i - fit_n_, x, y);
        if (!got) continue;
        if (!fit_ok_) {
            fit_min_x_ = fit_max_x_ = x;
            fit_min_y_ = fit_max_y_ = y;
            fit_ok_ = true;
            continue;
        }
        if (x < fit_min_x_) fit_min_x_ = x;
        if (x > fit_max_x_) fit_max_x_ = x;
        if (y < fit_min_y_) fit_min_y_ = y;
        if (y > fit_max_y_) fit_max_y_ = y;
    }
}

void MapView::render(Canvas& c) {
    if (fit_stale_) refit();       // a screen that never called set_live()

    c.fill_rect(bounds, color::kPanel);
    c.draw_rect(bounds, color::kGrid);

    const size_t n = fit_n_ >= 2 ? fit_n_ : 0;
    if (n < 2 && fit_route_n_ == 0) {
        c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - 3),
                    "no trace", color::kDim, 1, Align::Center);
        return;
    }
    if (!fit_ok_) return;

    const float span_x = (fit_max_x_ - fit_min_x_) > 1.0f ? (fit_max_x_ - fit_min_x_) : 1.0f;
    const float span_y = (fit_max_y_ - fit_min_y_) > 1.0f ? (fit_max_y_ - fit_min_y_) : 1.0f;
    // A margin the rider's marker fits in whole: the newest point is often
    // on the edge of the fit (the rider is what extends it), and its pointer
    // reaches 11 px out from there.
    const Rect in = bounds.inset(kMarkerMargin);
    const float sx = in.w / span_x;
    const float sy = in.h / span_y;
    const float s = sx < sy ? sx : sy;          // square, or the track lies
    const float mid_x = (fit_min_x_ + fit_max_x_) / 2;
    const float mid_y = (fit_min_y_ + fit_max_y_) / 2;

    // The route view fits the route, so the trace can run off the frame.
    const ClipGuard frame_clip(c, bounds.inset(1));

    // The route first, two pixels wide and dim (bright in the route view),
    // its 65 mph stretches amber, so the ridden trace reads on top of it.
    {
        const Color line = route_only_ ? color::kAccent : color::kDim;
        int16_t qx = 0, qy = 0;
        bool prev = false;
        for (size_t i = 0; i < fit_route_n_; ++i) {
            float x, y;
            bool fast;
            if (!route_->route_point(i, x, y, fast)) { prev = false; continue; }
            const int16_t dx = static_cast<int16_t>(in.cx() + (x - mid_x) * s);
            const int16_t dy = static_cast<int16_t>(in.cy() - (y - mid_y) * s);
            if (prev) {
                const Rect seg{static_cast<int16_t>((qx < dx ? qx : dx) - 1),
                               static_cast<int16_t>((qy < dy ? qy : dy) - 1),
                               static_cast<int16_t>((qx < dx ? dx - qx : qx - dx) + 3),
                               static_cast<int16_t>((qy < dy ? dy - qy : qy - dy) + 3)};
                if (c.intersects(seg)) {
                    const Color k = fast ? color::kWarn : line;
                    // The second pixel across the line: beside a steep one, under a flat one.
                    const bool flat = (qx < dx ? dx - qx : qx - dx) > (qy < dy ? dy - qy : qy - dy);
                    const int16_t ox = flat ? 0 : 1, oy = flat ? 1 : 0;
                    c.draw_line(qx, qy, dx, dy, k);
                    c.draw_line(static_cast<int16_t>(qx + ox), static_cast<int16_t>(qy + oy),
                                static_cast<int16_t>(dx + ox), static_cast<int16_t>(dy + oy), k);
                }
            }
            qx = dx;
            qy = dy;
            prev = true;
        }
        // Where it ends.
        if (route_only_ && prev) {
            c.draw_circle(qx, qy, 6, color::kText);
            c.draw_circle(qx, qy, 5, color::kText);
        }
    }

    // The route's main roads, most time first: a dot in the middle of each
    // one's longest stretch and the name beside it, on the side with room.
    // One with no room by it beside the others keeps its dot.
    if (route_only_) {
        constexpr uint8_t kScale = 2;
        constexpr int16_t kH = 7 * kScale, kGap = 5;
        Rect placed[core::state::Nav::kMaxLabels];
        uint8_t n_placed = 0;
        const Rect box = bounds.inset(2);
        for (uint8_t i = 0; i < n_labels_; ++i) {
            const core::state::Nav::Label& l = labels_[i];
            const int16_t dx = static_cast<int16_t>(in.cx() + (l.x_ft - mid_x) * s);
            const int16_t dy = static_cast<int16_t>(in.cy() - (l.y_ft - mid_y) * s);
            c.fill_circle(dx, dy, 3, color::kText);
            // Right, left, above, below the dot; then the same, half size.
            // None free: the dot alone, so the road still shows it was picked.
            for (uint8_t tryi = 0; tryi < 8; ++tryi) {
                const uint8_t sc = tryi < 4 ? kScale : 1;
                const int16_t w = text_width_px(l.name, sc), h = static_cast<int16_t>(kH * sc / kScale);
                int16_t tx = 0, ty = 0;
                switch (tryi % 4) {
                    case 0: tx = static_cast<int16_t>(dx + kGap);     ty = static_cast<int16_t>(dy - h / 2); break;
                    case 1: tx = static_cast<int16_t>(dx - kGap - w); ty = static_cast<int16_t>(dy - h / 2); break;
                    case 2: tx = static_cast<int16_t>(dx - w / 2);    ty = static_cast<int16_t>(dy - kGap - h); break;
                    default: tx = static_cast<int16_t>(dx - w / 2);   ty = static_cast<int16_t>(dy + kGap); break;
                }
                if (tx < box.x || ty < box.y || tx + w > box.x + box.w || ty + h > box.y + box.h) continue;
                const Rect r{static_cast<int16_t>(tx - 1), static_cast<int16_t>(ty - 1),
                             static_cast<int16_t>(w + 2), static_cast<int16_t>(h + 2)};
                bool clash = false;
                for (uint8_t k = 0; k < n_placed; ++k) clash = clash || r.intersects(placed[k]);
                if (clash) continue;
                placed[n_placed++] = r;
                if (c.intersects(r)) {
                    c.fill_rect(r, color::kPanel);   // legible over the line
                    c.draw_text(tx, ty, l.name, color::kText, sc);
                }
                break;
            }
        }
    }
    if (n < 2) return;

    int16_t px = 0, py = 0;
    bool have_prev = false;
    for (size_t i = 0; i < n; ++i) {
        float x, y;
        if (!src_->point(i, x, y)) { have_prev = false; continue; }
        const int16_t dx = static_cast<int16_t>(in.cx() + (x - mid_x) * s);
        // Screen y grows downwards; north does not.
        const int16_t dy = static_cast<int16_t>(in.cy() - (y - mid_y) * s);
        if (have_prev) {
            // Only the segments that reach this strip; the rest would be
            // walked pixel by pixel and every one of them thrown away.
            const Rect seg{px < dx ? px : dx, py < dy ? py : dy,
                           static_cast<int16_t>((px < dx ? dx - px : px - dx) + 1),
                           static_cast<int16_t>((py < dy ? dy - py : py - dy) + 1)};
            if (c.intersects(seg)) c.draw_line(px, py, dx, dy, color::kAccent);
        }
        px = dx;
        py = dy;
        have_prev = true;
    }

    float hx, hy;
    if (valid_ && src_->point(n - 1, hx, hy)) {
        const int16_t bx = static_cast<int16_t>(in.cx() + (hx - mid_x) * s);
        const int16_t by = static_cast<int16_t>(in.cy() - (hy - mid_y) * s);

        // Which way it was going: from a point far enough back that GPS
        // jitter between neighbouring fixes does not swing it -- 15 ft, and
        // 8 px on this map's scale -- to the newest. None that far back (a
        // trace just begun) is a dot without a pointer.
        const float min_ft = 8.0f / s > 15.0f ? 8.0f / s : 15.0f;
        float ux = 0.0f, uy = 0.0f;
        bool heading = false;
        for (size_t k = n - 1; k-- > 0;) {
            float x, y;
            if (!src_->point(k, x, y)) continue;
            const float ex = hx - x, ey = hy - y;
            const float d = std::sqrt(ex * ex + ey * ey);
            if (d >= min_ft) {
                ux = ex / d;
                uy = -ey / d;   // north up, screen y down
                heading = true;
                break;
            }
        }

        // Inside the frame: a pointer near the edge must not draw where the
        // next redraw of this widget would not clear it.
        const Color bike = route_only_ ? color::kText : color::kWarn;
        if (heading) {
            // An arrowhead ahead of the dot: tip 11 px out, base corners 6 px
            // either side, 2 px behind the centre, so it reads as the dot's
            // nose rather than a separate mark.
            const float tip = 11.0f, back = 2.0f, half = 6.0f;
            const auto px_at = [&](float along, float side, int16_t& ox, int16_t& oy) {
                ox = static_cast<int16_t>(std::lround(bx + ux * along - uy * side));
                oy = static_cast<int16_t>(std::lround(by + uy * along + ux * side));
            };
            int16_t x0, y0, x1, y1, x2, y2;
            px_at(tip, 0.0f, x0, y0);
            px_at(-back, half, x1, y1);
            px_at(-back, -half, x2, y2);
            c.fill_triangle(x0, y0, x1, y1, x2, y2, bike);
        }
        c.fill_circle(bx, by, 3, bike);
    }
}

// --- TachBar ------------------------------------------------------------

int16_t TachBar::width_at(int16_t y) {
    if (y >= kFlareH) return kStemW;
    if (y <= 0) return kFlareW;
    // Linear from kStemW at the bottom of the flare to kFlareW at the top.
    // At y == kFlareH this is kStemW, which is what the straight run above
    // returns, so the outline has no step in it anywhere.
    return static_cast<int16_t>(kStemW +
                                (kFlareH - y) * (kFlareW - kStemW) / kFlareH);
}

// --- RevBar -------------------------------------------------------------

void RevBar::set_scale(uint16_t full_rpm) {
    if (assign_t(full_rpm_, full_rpm)) edge_ = edge_for(-1.0f);
}

int16_t RevBar::edge_for(float rpm) const {
    if (full_rpm_ == 0 || rpm <= 0.0f) return 0;
    float frac = rpm / static_cast<float>(full_rpm_);
    if (frac > 1.0f) frac = 1.0f;
    const int16_t e = static_cast<int16_t>(frac * bounds.w);
    return static_cast<int16_t>((e / kStepPx) * kStepPx);
}

void RevBar::set(float rpm, bool lamps, bool flash, bool valid) {
    // The number first: it changes on its own 100 rpm steps, and only its
    // box goes out when that is all that moved.
    const int32_t n = number_ && valid && rpm > 0.0f
                          ? static_cast<int32_t>(rpm / 100.0f + 0.5f) * 100 : -1;
    if (n != number_rpm_) {
        number_rpm_ = n;
        if (number_) touch_part();
    }
    const Shade shade = flash ? Shade::Flash : lamps ? Shade::Lamps : Shade::Normal;
    // A change of colour or of validity repaints the lot; only a moving
    // edge is worth working out the columns for.
    if (assign_t(shade_, shade) | assign_t(valid_, valid)) {
        edge_ = valid ? edge_for(rpm) : 0;
        return;
    }
    const int16_t e = valid ? edge_for(rpm) : 0;
    if (e == edge_) return;
    int16_t l = e < edge_ ? e : edge_;
    int16_t r = e < edge_ ? edge_ : e;
    if (l < 0) l = 0;
    edge_ = e;
    if (moved_) {
        if (moved_l_ < l) l = moved_l_;
        if (moved_r_ > r) r = moved_r_;
    }
    moved_ = true;
    moved_l_ = l;
    moved_r_ = r;
    touch_part();
}

Rect RevBar::number_box() const {
    const int16_t pad = 4;
    const int16_t h = static_cast<int16_t>(7 * kNumberScale + 2 * pad);
    const int16_t w = static_cast<int16_t>(text_width_px("12000", kNumberScale) + 2 * pad);
    return Rect{static_cast<int16_t>(bounds.x + 6),
                static_cast<int16_t>(bounds.y + (bounds.h - h) / 2), w, h};
}

void RevBar::show_number(bool on) {
    if (assign_t(number_, on)) touch();
}

Rect RevBar::peak_cols(int16_t x) const {
    if (x < 0) return Rect{};
    return Rect{static_cast<int16_t>(bounds.x + x - 1), bounds.y, 3, bounds.h}.clipped_to(bounds);
}

void RevBar::set_peak(float rpm) {
    int16_t x = rpm > 0.0f ? edge_for(rpm) : -1;
    if (x >= bounds.w) x = static_cast<int16_t>(bounds.w - 2);
    if (x == peak_x_) return;
    peak_x_ = x;
    touch_part();
}

int RevBar::damage(Rect* out, int max) const {
    if (whole_ || max < 3) { out[0] = bounds; return 1; }
    int n = 0;
    if (moved_) {
        const Rect r = Rect{static_cast<int16_t>(bounds.x + moved_l_), bounds.y,
                            static_cast<int16_t>(moved_r_ - moved_l_), bounds.h}.clipped_to(bounds);
        if (!r.empty()) out[n++] = r;
    }
    if (peak_x_ != peak_sent_x_) {
        const Rect was = peak_cols(peak_sent_x_), now = peak_cols(peak_x_);
        if (!was.empty()) out[n++] = was;
        if (!now.empty()) out[n++] = now;
    }
    if (number_ && number_rpm_ != number_sent_ && n < max) out[n++] = number_box();
    return n;
}

void RevBar::render(Canvas& c) {
    const int16_t e = edge_ < 0 ? 0 : (edge_ > bounds.w ? bounds.w : edge_);
    const Color lit = shade_ == Shade::Flash ? color::kAlarm
                    : shade_ == Shade::Lamps ? color::kWarn
                                             : color::kAccent;
    if (e > 0) c.fill_rect(Rect{bounds.x, bounds.y, e, bounds.h}, lit);
    if (e < bounds.w) {
        c.fill_rect(Rect{static_cast<int16_t>(bounds.x + e), bounds.y,
                         static_cast<int16_t>(bounds.w - e), bounds.h},
                    color::kPanel);
    }
    if (full_rpm_ == 0) return;
    // The thousands, full height so they read across the fill and the track
    // alike; every fifth one thicker.
    for (uint32_t rpm = 1000; rpm < full_rpm_; rpm += 1000) {
        const int16_t x = static_cast<int16_t>(bounds.x + static_cast<int32_t>(rpm) * bounds.w /
                                                              full_rpm_);
        const int16_t w = rpm % 5000 == 0 ? 3 : 1;
        c.fill_rect(Rect{static_cast<int16_t>(x - w / 2), bounds.y, w, bounds.h}, color::kBg);
    }
    // The held peak over the bar: white, as on SPEED's.
    if (valid_ && peak_x_ >= 0) c.fill_rect(peak_cols(peak_x_), color::kWhite);
    // The number over everything, in a box of its own so it reads over the
    // fill, the track and the marks alike.
    if (number_ && valid_) {
        const Rect b = number_box();
        c.fill_round_rect(b, 4, color::kBg);
        char num[12];
        if (number_rpm_ >= 0) std::snprintf(num, sizeof num, "%d", static_cast<int>(number_rpm_));
        else std::snprintf(num, sizeof num, "0");
        c.draw_text(static_cast<int16_t>(b.right() - 4), static_cast<int16_t>(b.y + 4), num,
                    color::kWhite, kNumberScale, Align::Right);
    }
    if (!valid_) {
        c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - 7), "--", color::kDim, 2,
                    Align::Center);
    }
}

void TachBar::configure(int16_t y_top, int16_t y_bottom) {
    const bool a = assign_t(y_top_, y_top);
    const bool b = assign_t(y_bottom_, y_bottom);
    if (a || b) touch();
}

bool TachBar::covers(int16_t marker_y) const {
    return marker_y + kMarkerH > y_top_ && marker_y < y_bottom_;
}

void TachBar::set(float fraction) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    // The marker travels the whole height of the panel, so full scale puts
    // its top edge on the top row rather than somewhere near it.
    const int16_t travel = static_cast<int16_t>(kScreenH - kMarkerH);
    int16_t y = static_cast<int16_t>(travel - fraction * travel);
    y = static_cast<int16_t>((y / kStepPx) * kStepPx);
    if (y == marker_y_) return;

    // Only the strip the marker is leaving or arriving in has changed. The
    // other one is still the same rows of unlit track it was before.
    const bool was = covers(marker_y_);
    const bool now = covers(y);
    const int16_t old = marker_y_;
    marker_y_ = y;
    if (!was && !now) return;

    // Only the rows between the old marker and the new one, within this
    // instance's strip, have changed.
    int16_t top = y;
    int16_t bottom = static_cast<int16_t>(y + kMarkerH);
    if (was) {
        if (old < top) top = old;
        if (old + kMarkerH > bottom) bottom = static_cast<int16_t>(old + kMarkerH);
    }
    if (top < y_top_) top = y_top_;
    if (bottom > y_bottom_) bottom = y_bottom_;
    if (moved_) {
        if (moved_top_ < top) top = moved_top_;
        if (moved_bottom_ > bottom) bottom = moved_bottom_;
    }
    moved_ = true;
    moved_top_ = top;
    moved_bottom_ = bottom;
    touch_part();
}

Rect TachBar::peak_rows(int16_t y) const {
    if (y == kNoPeak) return Rect{};
    return Rect{bounds.x, y, bounds.w, kPeakH}.clipped_to(
        Rect{bounds.x, y_top_, bounds.w, static_cast<int16_t>(y_bottom_ - y_top_)});
}

void TachBar::set_peak(float fraction) {
    int16_t y = kNoPeak;
    if (fraction > 0.0f) {
        if (fraction > 1.0f) fraction = 1.0f;
        // Where the marker's top edge was at that rpm: the line sits on it.
        const int16_t travel = static_cast<int16_t>(kScreenH - kMarkerH);
        y = static_cast<int16_t>(travel - fraction * travel);
        y = static_cast<int16_t>((y / kStepPx) * kStepPx);
    }
    if (y == peak_y_) return;
    const bool mine = !peak_rows(peak_y_).empty() || !peak_rows(y).empty();
    peak_y_ = y;
    if (mine) touch_part();
}

int TachBar::damage(Rect* out, int max) const {
    if (whole_ || max < 3) { out[0] = bounds; return 1; }
    int n = 0;
    if (moved_) {
        const Rect r = Rect{bounds.x, moved_top_, bounds.w,
                            static_cast<int16_t>(moved_bottom_ - moved_top_)}.clipped_to(bounds);
        if (!r.empty()) out[n++] = r;
    }
    // The held peak where it was on the panel and where it is: a line a
    // shift leaves and the hold takes away, 3 rows each, not the bar.
    if (peak_y_ != peak_sent_y_) {
        const Rect was = peak_rows(peak_sent_y_), now = peak_rows(peak_y_);
        if (!was.empty()) out[n++] = was;
        if (!now.empty()) out[n++] = now;
    }
    return n;
}

void TachBar::set_scale(uint16_t base_rpm) {
    if (base_rpm == base_rpm_) return;
    base_rpm_ = base_rpm;

    const int16_t travel = static_cast<int16_t>(kScreenH - kMarkerH);
    tick_count_ = 0;
    for (uint32_t rpm = 1000; rpm < base_rpm && tick_count_ < kMaxTicks;
         rpm += 1000) {
        const float frac = static_cast<float>(rpm) / base_rpm;
        tick_y_[tick_count_++] =
            static_cast<int16_t>(travel - frac * travel);
    }
    touch();
}

void TachBar::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    for (int16_t y = y_top_; y < y_bottom_; ++y) {
        const bool lit = y >= marker_y_ &&
                         y < static_cast<int16_t>(marker_y_ + kMarkerH);
        c.fill_rect(Rect{bounds.x, y, width_at(y), 1},
                    lit ? color::kTach : color::kTachTrack);
    }
    // Drawn after the marker, not before: a mark this widget's row range
    // owns must stay visible even when the marker is riding over it.
    for (uint8_t i = 0; i < tick_count_; ++i) {
        for (int16_t y = tick_y_[i]; y < tick_y_[i] + kTickH; ++y) {
            if (y < y_top_ || y >= y_bottom_) continue;
            c.fill_rect(Rect{bounds.x, y, width_at(y), 1}, color::kAlarm);
        }
    }
    // Last, over the marker and the thousands: white, the one colour on the
    // bar that is neither, so it reads as "you were here" at a glance.
    if (peak_y_ != kNoPeak) {
        for (int16_t y = peak_y_; y < peak_y_ + kPeakH; ++y) {
            if (y < y_top_ || y >= y_bottom_) continue;
            c.fill_rect(Rect{bounds.x, y, width_at(y), 1}, color::kWhite);
        }
    }
}

// --- LeanGauge ----------------------------------------------------------

void LeanGauge::set(float lean_deg, bool valid) {
    // Half a degree is finer than the arc can draw at this radius, and the
    // hold keeps noise from walking it back and forth.
    valid = usable(lean_deg, valid);
    const bool was_valid = valid_;
    if (valid_ != valid) {
        valid_ = valid;
        touch();
    }
    if (valid) {
        const float h = held_lean(lean_deg_, was_valid, lean_deg);
        if (h != lean_deg_) {
            lean_deg_ = h;
            touch_part();       // the marker and the number; see damage()
        }
    }
}

LeanGauge::Geom LeanGauge::geom() const {
    // render()'s arithmetic, exactly; see the comments there.
    Geom g;
    const int16_t by = static_cast<int16_t>((bounds.h - 4) * 20 / 21);
    const int16_t bx = static_cast<int16_t>((bounds.w - 12) / 2);
    g.r = bx < by ? bx : by;
    const int16_t t = static_cast<int16_t>(g.r / 6);
    g.ov = static_cast<int16_t>(g.r / 20);
    g.ri = static_cast<int16_t>(g.r - t);
    g.cx = bounds.cx();
    g.cy = static_cast<int16_t>(bounds.y + g.r + g.ov + 1);
    g.scale = static_cast<uint8_t>(g.ri / 16);
    if (g.scale < 2) g.scale = 2;
    return g;
}

Rect LeanGauge::marker_box(const Geom& g, float lean_deg) const {
    float a = lean_deg;
    if (a < -85.0f) a = -85.0f;
    if (a > 85.0f) a = 85.0f;
    // Every degree of the marker's 14, on both its edges: the box of the
    // sector, without working out which axes it crosses.
    const float r_out = static_cast<float>(g.r + g.ov);
    const float r_in = static_cast<float>(g.ri - g.ov);
    Box b;
    for (int k = -7; k <= 7; ++k) {
        const float rad = (a + static_cast<float>(k)) * kPi / 180.0f;
        const float sx = std::sin(rad), cy = std::cos(rad);
        b.add(static_cast<int16_t>(std::lround(g.cx + r_out * sx)),
              static_cast<int16_t>(std::lround(g.cy - r_out * cy)));
        b.add(static_cast<int16_t>(std::lround(g.cx + r_in * sx)),
              static_cast<int16_t>(std::lround(g.cy - r_in * cy)));
    }
    return b.rect(3).clipped_to(bounds);
}

Rect LeanGauge::number_box(const Geom& g) const {
    // Up to "85", or "--": three characters is room to spare either way.
    const int16_t w = text_width_px("888", g.scale);
    const int16_t h = static_cast<int16_t>(font_5x7().height * g.scale);
    return Rect{static_cast<int16_t>(g.cx - w / 2 - g.scale),
                static_cast<int16_t>(g.cy - 7 * g.scale - 2),
                static_cast<int16_t>(w + 2 * g.scale), h}.clipped_to(bounds);
}

int LeanGauge::damage(Rect* out, int max) const {
    if (whole_ || face_ != Face::Lean || !valid_ || max < 3) {
        out[0] = bounds;
        return 1;
    }
    const Geom g = geom();
    int n = 0;
    const Rect parts[3] = {marker_box(g, sent_lean_), marker_box(g, lean_deg_), number_box(g)};
    for (const Rect& r : parts) if (!r.empty()) out[n++] = r;
    return n;
}

void LeanGauge::set_jump(Face face, uint32_t airtime_ms, float landing_g) {
    if (face == Face::Air) airtime_ms -= airtime_ms % 100;
    const bool a = assign_t(face_, face);
    const bool b = face != Face::Lean && assign_t(airtime_ms_, airtime_ms);
    const bool d = face == Face::Landed &&
                   assign(landing_g_, std::round(landing_g * 10.0f) / 10.0f);
    if (a || b || d) touch();
}

void LeanGauge::set_trick(Face face, const char* label, Color label_color, const char* big,
                          const char* detail) {
    const bool a = assign_t(face_, face);
    const bool b = trick_label_.set(label);
    const bool d = assign_t(trick_color_, label_color);
    const bool e = trick_big_.set(big);
    const bool g = trick_detail_.set(detail);
    // Under way the detail changes nearly every frame (the points run up);
    // it goes out with the next tenth of the time instead.
    if (a || b || d || e || (g && face == Face::TrickDone)) touch();
}

void LeanGauge::render_trick(Canvas& c) {
    // The jump face's layout: a label, the number big, a line under it.
    const int16_t cx = bounds.cx();
    c.draw_text(cx, static_cast<int16_t>(bounds.y + 4), trick_label_.s, trick_color_, 2,
                Align::Center);
    c.draw_text(cx, static_cast<int16_t>(bounds.y + 24), trick_big_.s,
                face_ == Face::Trick ? color::kAccent : color::kWhite, 6, Align::Center);
    c.draw_text(cx, static_cast<int16_t>(bounds.y + 88), trick_detail_.s, color::kText, 2,
                Align::Center);
}

void LeanGauge::render_jump(Canvas& c) {
    // Big enough to read from the air, which is the only time the first
    // face is ever up. Laid out for the 248x126 middle band: a label, the
    // number, and on the ground the landing under it.
    const int16_t cx = bounds.cx();
    const bool air = face_ == Face::Air;
    c.draw_text(cx, static_cast<int16_t>(bounds.y + 4), air ? "AIR" : "AIRTIME",
                color::kDim, 2, Align::Center);

    char num[12];
    core::units::fmt_float(num, sizeof num,
                           static_cast<float>(airtime_ms_) / 1000.0f, air ? 1 : 2);
    const uint8_t ns = 6;
    const int16_t nw = c.text_width(num, ns);
    const int16_t uw = c.text_width("s", 3);
    const int16_t x0 = static_cast<int16_t>(cx - (nw + 6 + uw) / 2);
    const int16_t ty = static_cast<int16_t>(bounds.y + 24);
    c.draw_text(x0, ty, num, air ? color::kAccent : color::kWhite, ns, Align::Left);
    c.draw_text(static_cast<int16_t>(x0 + nw + 6),
                static_cast<int16_t>(ty + c.text_height(ns) - c.text_height(3)),
                "s", color::kDim, 3, Align::Left);
    if (air) return;

    // The landing: amber from 4 g, which is a landing worth knowing about.
    char land[24];
    size_t k = core::units::fmt_float(land, sizeof land, landing_g_, 1);
    for (const char* q = " g  LAND"; *q && k + 1 < sizeof land; ++q) land[k++] = *q;
    land[k] = '\0';
    c.draw_text(cx, static_cast<int16_t>(bounds.y + 84), land,
                landing_g_ >= 4.0f ? color::kWarn : color::kText, 3, Align::Center);
}

void LeanGauge::render(Canvas& c) {
    // Nothing outside its own box: at a lean near the end stops the marker's
    // corner reached a row below it, which a partial update of this widget
    // never clears (found by test_ui's random ride).
    const ClipGuard clip(c, bounds);
    c.fill_rect(bounds, color::kBg);
    if (face_ == Face::Trick || face_ == Face::TrickDone) {
        render_trick(c);
        return;
    }
    if (face_ != Face::Lean) {
        render_jump(c);
        return;
    }

    // Everything scales off the radius, which is as big as the bounds allow:
    // the track a sixth of it, the marker standing a twentieth proud of the
    // track on both edges, and the pivot low enough that the marker's outer
    // edge stays inside the bounds -- a repaint clears only the bounds, so a
    // marker poking out above them would leave its old position behind.
    const int16_t by = static_cast<int16_t>((bounds.h - 4) * 20 / 21);
    const int16_t bx = static_cast<int16_t>((bounds.w - 12) / 2);
    const int16_t r  = bx < by ? bx : by;
    const int16_t t  = static_cast<int16_t>(r / 6);
    const int16_t ov = static_cast<int16_t>(r / 20);
    const int16_t ri = static_cast<int16_t>(r - t);
    const int16_t cx = bounds.cx();
    const int16_t cy = static_cast<int16_t>(bounds.y + r + ov + 1);

    c.fill_arc(cx, cy, r, ri, 270.0f, 360.0f, color::kGrid);
    c.fill_arc(cx, cy, r, ri, 0.0f, 90.0f, color::kGrid);

    if (valid_) {
        float a = lean_deg_;
        if (a < -85.0f) a = -85.0f;
        if (a > 85.0f) a = 85.0f;
        // Leaning right puts the marker right, which is where the rider is
        // going. Zero is straight up.
        const float mark = a < 0.0f ? 360.0f + a : a;
        c.fill_arc(cx, cy, static_cast<int16_t>(r + ov),
                   static_cast<int16_t>(ri - ov), mark - 7.0f, mark + 7.0f,
                   color::kTach);
    }

    // The number inside the arc, sitting on the pivot line, as big as the
    // arc's opening takes: a sixth of the radius per unit of scale.
    char buf[16];
    core::units::fmt_value(buf, sizeof buf,
                           lean_deg_ < 0 ? -lean_deg_ : lean_deg_, 0, valid_);
    uint8_t scale = static_cast<uint8_t>(ri / 16);
    if (scale < 2) scale = 2;
    c.draw_text(cx, static_cast<int16_t>(cy - 7 * scale - 2), buf,
                valid_ ? color::kText : color::kDim, scale, Align::Center);
}

// --- MediaRow -----------------------------------------------------------

namespace {

const icon::Bitmap& icon_for(MediaRow::Kind k) {
    switch (k) {
        case MediaRow::Kind::Album:  return icon::kAlbum;
        case MediaRow::Kind::Artist: return icon::kArtist;
        case MediaRow::Kind::Lyric:  return icon::kLyric;
        case MediaRow::Kind::Song:   break;
    }
    return icon::kSong;
}

}  // namespace

void MediaRow::configure(Kind k, Color text_color, uint8_t scale, bool wrap) {
    const bool a = assign_t(kind_, k);
    const bool b = assign_t(color_, text_color);
    const bool d = assign_t(scale_, scale);
    const bool e = assign_t(wrap_, wrap);
    if (a || b || d || e) touch();
}

namespace {
// How many leading characters of `t` go on the first of two lines `n` wide:
// up to the last space that fits, or a hard break when one word is longer.
size_t wrap_at(const char* t, size_t len, size_t n) {
    if (len <= n) return len;
    for (size_t i = n; i > 0; --i) {
        if (t[i] == ' ') return i;
    }
    return n;
}
}  // namespace

void MediaRow::set(const char* text, bool valid, uint32_t now_ms) {
    bool changed = text_.set(text ? text : "");
    changed = assign_t(valid_, valid) || changed;

    const int16_t avail = static_cast<int16_t>(bounds.w - kTextX);
    const int16_t w = text_width_px(text_.s, scale_);

    int16_t off = 0;
    if (!wrap_ && valid_ && w > avail && avail > 0) {
        const uint32_t cycle = static_cast<uint32_t>(w) + kGapPx;
        // Split so the multiply cannot overflow a 32-bit millisecond counter
        // that has been running for a month.
        uint32_t travelled = (now_ms / 1000u) * kSpeedPxPerS +
                             ((now_ms % 1000u) * kSpeedPxPerS) / 1000u;
        // Each row starts a third of a cycle further along than the one
        // above it. Two rows that both overflow otherwise slide in
        // lockstep, because they are driven off the same frame clock, and
        // three lines moving as one block reads as a glitch rather than as
        // three names that happen to be long.
        travelled += (static_cast<uint32_t>(kind_) * cycle) / 3u;
        travelled %= cycle;
        off = static_cast<int16_t>((travelled / kStepPx) * kStepPx);
    }
    changed = assign_t(offset_, off) || changed;

    if (changed) touch();
}

void MediaRow::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    if (!valid_ || text_.empty()) return;

    const icon::Bitmap& ic = icon_for(kind_);
    c.draw_bitmap(bounds.x, bounds.y, ic.bits, ic.w, ic.h, color::kDim);

    const int16_t tx = static_cast<int16_t>(bounds.x + kTextX);
    const int16_t ty = static_cast<int16_t>(bounds.y + 1);
    const int16_t avail = static_cast<int16_t>(bounds.w - kTextX);
    const int16_t w = text_width_px(text_.s, scale_);

    // Everything below draws deliberately wider than the window it is in.
    const ClipGuard clip(c, Rect{tx, bounds.y, avail, bounds.h});

    // The lyric is read as a sentence, so it gets the smoothed small font;
    // the other rows, and every other small text, keep the bitmap.
    struct SmoothGuard {
        Canvas& c;
        bool on;
        SmoothGuard(Canvas& cv, bool o) : c(cv), on(o) { if (on) c.set_smooth_small(true); }
        ~SmoothGuard() { if (on) c.set_smooth_small(false); }
    } smooth(c, kind_ == Kind::Lyric);

    if (wrap_) {
        const Font& f = font_5x7();
        const size_t n = static_cast<size_t>(
            (avail / scale_ + (f.advance - f.width)) / f.advance);
        const size_t len = std::strlen(text_.s);
        const size_t cut = wrap_at(text_.s, len, n);
        char line[sizeof text_.s];
        std::memcpy(line, text_.s, cut);
        line[cut] = '\0';
        c.draw_text(tx, ty, line, color_, scale_, Align::Left);
        const char* rest = text_.s + cut;
        while (*rest == ' ') ++rest;
        if (*rest) {
            c.draw_text(tx, static_cast<int16_t>(ty + kWrapLineH * scale_),
                        rest, color_, scale_, Align::Left);
        }
        return;
    }

    if (w <= avail) {
        c.draw_text(tx, ty, text_.s, color_, scale_, Align::Left);
        return;
    }

    // Twice, a cycle apart, so the tail of one pass is followed by the head
    // of the next and the belt never shows a hole.
    const int16_t cycle = static_cast<int16_t>(w + kGapPx);
    c.draw_text(static_cast<int16_t>(tx - offset_), ty, text_.s, color_,
                scale_, Align::Left);
    c.draw_text(static_cast<int16_t>(tx - offset_ + cycle), ty, text_.s,
                color_, scale_, Align::Left);
}

// --- AlbumArt --------------------------------------------------------------

void AlbumArt::set(const char* album, bool valid) {
    const bool a = album_.set(album ? album : "");
    const bool b = assign_t(valid_, valid);
    if (a || b) {
        cover_ = (valid_ && !album_.empty()) ? album_art::find(album_.s)
                                              : nullptr;
        touch();
    }
}

void AlbumArt::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    if (!cover_) return;
    // Centred rather than stretched: the source is baked at a fixed size
    // (`tools/make_album_art.py`), and there is no scaling primitive to
    // fit an arbitrary tile — see Canvas::draw_image.
    const int16_t x = static_cast<int16_t>(bounds.x + (bounds.w - cover_->w) / 2);
    const int16_t y = static_cast<int16_t>(bounds.y + (bounds.h - cover_->h) / 2);
    c.draw_image(x, y, cover_->pixels, cover_->w, cover_->h);
}

// --- MediaProgress --------------------------------------------------------

namespace {

// Plain "M:SS", no tenths and no hour digit — this is a song, not a lap.
// Integer maths rather than snprintf, matching core/units — Clamped to
// 99:59 rather than growing a third digit, which nothing that plays music
// would ever actually reach.
void fmt_mmss(char* buf, size_t n, uint32_t ms) {
    uint32_t total_s = ms / 1000;
    if (total_s > 99 * 60 + 59) total_s = 99 * 60 + 59;
    const uint32_t m = total_s / 60;
    const uint32_t s = total_s % 60;
    size_t k = 0;
    if (m >= 10 && k + 1 < n) buf[k++] = static_cast<char>('0' + (m / 10) % 10);
    if (k + 1 < n) buf[k++] = static_cast<char>('0' + m % 10);
    if (k + 1 < n) buf[k++] = ':';
    if (k + 1 < n) buf[k++] = static_cast<char>('0' + s / 10);
    if (k + 1 < n) buf[k++] = static_cast<char>('0' + s % 10);
    buf[k < n ? k : n - 1] = '\0';
}

}  // namespace

void MediaProgress::set(uint32_t elapsed_ms, uint32_t duration_ms,
                        uint8_t state, bool valid) {
    // To the second: at a 20-50 Hz update rate this is the difference
    // between a ring that repaints once a second and one that repaints
    // every tick for a hand that has not visibly moved.
    const bool a = assign_t(elapsed_ms_, elapsed_ms - elapsed_ms % 1000);
    const bool b = assign_t(duration_ms_, duration_ms);
    const bool d = assign_t(state_, state);
    const bool e = assign_t(valid_, valid);
    if (a || b || d || e) touch();
}

void MediaProgress::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);

    const int16_t cx = bounds.cx();
    const int16_t cy = bounds.cy();
    const int16_t r  = static_cast<int16_t>(bounds.w < bounds.h
                            ? bounds.w / 2 - 2 : bounds.h / 2 - 2);

    // Every pixel offset below scales off r relative to the tile this was
    // first drawn at (r = 38, SpeedScreen's 80x80 kMediaArc), so a bigger
    // ring elsewhere — MediaScreen's is roughly twice the radius — gets a
    // thicker band, a bigger icon and bigger time text instead of the same
    // fixed handful of pixels adrift in the middle of more circle. At
    // r == 38 every one of these divisions reduces to the original literal
    // exactly (38 * k / 38 == k), so SpeedScreen's tile is pixel-identical
    // to what it always drew.
    const int16_t band = static_cast<int16_t>(r * 6 / 38);
    const int16_t ri = static_cast<int16_t>(r - band);
    // A discrete choice, not something to compute a ratio for: text scale
    // is an integer multiplier, and there is no "1.9" to round to. Halfway
    // between SpeedScreen's r (38) and MediaScreen's (73) is as good a line
    // as any, and keeps SpeedScreen's own tile at the scale it always drew.
    const uint8_t txt_scale = static_cast<uint8_t>(r >= 55 ? 2 : 1);

    c.fill_arc(cx, cy, r, ri, 0.0f, 360.0f, color::kGrid);

    if (valid_ && duration_ms_ > 0) {
        float frac = static_cast<float>(elapsed_ms_) /
                    static_cast<float>(duration_ms_);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac > 0.0f) {
            c.fill_arc(cx, cy, r, ri, 0.0f, frac * 360.0f, color::kAccent);
        }
    }

    // Play is a right-pointing triangle, pause/stop is two bars — the
    // current state, not a button to tap; there is no touch on this tile.
    const int16_t iy = static_cast<int16_t>(cy - r * 16 / 38);
    const int16_t itri = static_cast<int16_t>(r * 4 / 38);
    const int16_t ivert = static_cast<int16_t>(r * 5 / 38);
    const int16_t ibarw = static_cast<int16_t>(r * 3 / 38 > 0 ? r * 3 / 38 : 1);
    const int16_t ibarh = static_cast<int16_t>(r * 10 / 38);
    if (valid_ && state_ == 1) {
        c.fill_triangle(static_cast<int16_t>(cx - itri), static_cast<int16_t>(iy - ivert),
                        static_cast<int16_t>(cx - itri), static_cast<int16_t>(iy + ivert),
                        static_cast<int16_t>(cx + ivert), iy, color::kText);
    } else {
        c.fill_rect(Rect{static_cast<int16_t>(cx - ivert), static_cast<int16_t>(iy - ivert),
                        ibarw, ibarh}, valid_ ? color::kText : color::kDim);
        c.fill_rect(Rect{static_cast<int16_t>(cx + itri - ibarw + 1),
                        static_cast<int16_t>(iy - ivert), ibarw, ibarh},
                   valid_ ? color::kText : color::kDim);
    }

    char elapsed[8] = "--:--", total[8] = "--:--";
    if (valid_) {
        fmt_mmss(elapsed, sizeof elapsed, elapsed_ms_);
        // 0 is "the phone hasn't said" (same convention as everywhere else
        // here), not a zero-length track — showing "0:00" would read as a
        // real answer instead of a missing one.
        if (duration_ms_ > 0) fmt_mmss(total, sizeof total, duration_ms_);
    }
    c.draw_text(cx, static_cast<int16_t>(cy - 2 * txt_scale), elapsed,
               valid_ ? color::kText : color::kDim, txt_scale, Align::Center);
    c.draw_text(cx, static_cast<int16_t>(cy + 8 * txt_scale), total, color::kDim,
               txt_scale, Align::Center);
}

// --- AlertOverlay -------------------------------------------------------

void AlertOverlay::show(const char* text, Color c, const char* hint) {
    const bool h = hint_.set(hint);
    const bool a = text_.set(text) | h;
    const bool b = assign_t(color_, c);
    const bool d = assign_t(active_, true);
    if (a || b || d) touch();
}

void AlertOverlay::dismiss() { if (assign_t(active_, false)) touch(); }

void AlertOverlay::render(Canvas& c) {
    if (!active_) return;
    c.fill_rect(bounds, color::kPanel);
    c.draw_rect(bounds, color_);
    c.draw_rect(bounds.inset(1), color_);
    c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - c.text_height(2) / 2),
                text_.s, color_, 2, Align::Center);
    c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.bottom() - c.text_height(1) - 4),
                hint_.s, color::kDim, 1, Align::Center);
}

// --- Toast --------------------------------------------------------------

namespace {
// At most `chars` bytes of `src`, cut on a UTF-8 boundary with "..." when
// cut: what fits across the strip at that line's scale.
void fit(char* dst, size_t chars, const char* src) {
    size_t len = std::strlen(src);
    if (len <= chars) {
        std::memcpy(dst, src, len + 1);
        return;
    }
    size_t n = chars > 3 ? chars - 3 : 0;
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, src, n);
    std::memcpy(dst + n, "...", 4);
}
// The strip is 480 px less a 16 px margin: 25 characters at scale 3, 38 at 2.
constexpr size_t kToastBigChars = 25, kToastSmallChars = 38;
}  // namespace

void Toast::show(const char* big, const char* small, Color c, const char* hint) {
    char b1[kToastBigChars + 1], s1[kToastSmallChars + 1];
    // A hint takes the top-right corner: four characters of the big line.
    fit(b1, hint && hint[0] ? kToastBigChars - 4 : kToastBigChars, big);
    fit(s1, kToastSmallChars, small);
    const bool h = hint_.set(hint);
    const bool a = big_.set(b1) || h;
    const bool b = small_.set(s1);
    const bool d = assign_t(color_, c);
    const bool e = assign_t(active_, true);
    if (a || b || d || e) touch();
}

void Toast::hide() { if (assign_t(active_, false)) touch(); }

void Toast::render(Canvas& c) {
    if (!active_) return;
    c.fill_rect(bounds, color::kPanel);
    // A bar of its colour down the left edge and a rule along the top: it
    // reads as a separate thing over the screen, not part of it.
    c.fill_rect(Rect{bounds.x, bounds.y, 6, bounds.h}, color_);
    c.fill_rect(Rect{bounds.x, bounds.y, bounds.w, 2}, color_);
    const int16_t x = static_cast<int16_t>(bounds.x + 16);
    c.draw_text(x, static_cast<int16_t>(bounds.y + 6), big_.s, color_, 3, Align::Left);
    if (!hint_.empty()) {
        c.draw_text(static_cast<int16_t>(bounds.right() - 8), static_cast<int16_t>(bounds.y + 7),
                    hint_.s, color::kDim, 1, Align::Right);
    }
    if (!small_.empty()) {
        c.draw_text(x, static_cast<int16_t>(bounds.y + 6 + c.text_height(3) + 5), small_.s,
                    color::kText, 2, Align::Left);
    }
}

// --- DevTag -------------------------------------------------------------

void SlipBar::set(core::slip::Kind kind, uint8_t pct) {
    const bool a = assign_t(kind_, kind);
    const bool b = assign_t(pct_, kind == core::slip::Kind::None ? uint8_t{0} : pct);
    if (a || b) touch();
}

void SlipBar::render(Canvas& c) {
    using core::slip::Kind;
    if (kind_ == Kind::None) {
        c.fill_rect(bounds, color::kBg);
        return;
    }
    const bool lock = kind_ != Kind::Spin;
    const Color fill = lock ? color::kAlarm : color::kWarn;
    // Full at 100% for a lock (a stopped wheel) and at 50% for spin, which
    // is already the rear doing half as much again as the bike.
    const int full = lock ? 100 : 50;
    int w = bounds.w * (pct_ < full ? pct_ : full) / full;
    if (w < 24) w = 24;            // always visibly there while it is shown
    c.fill_rect(bounds, color::kPanel);
    c.fill_rect(Rect{bounds.x, bounds.y, static_cast<int16_t>(w), bounds.h}, fill);

    char buf[24];
    size_t k = 0;
    for (const char* p = core::slip::kind_name(kind_); *p && k + 6 < sizeof buf; ++p) buf[k++] = *p;
    buf[k++] = ' ';
    k += core::units::fmt_value(buf + k, sizeof buf - k, pct_, 0, true);
    if (k + 1 < sizeof buf) buf[k++] = '%';
    buf[k] = '\0';
    const uint8_t scale = 3;
    // Dark on the colour once the fill reaches the words, white on the
    // panel before it: white on amber washes out in sun.
    const Color ink = w > bounds.w / 2 ? color::kBg : color::kWhite;
    c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - c.text_height(scale) / 2), buf,
                ink, scale, Align::Center);
}

void DevTag::render(Canvas& c) {
    c.fill_rect(bounds, color::kAlarm);
    c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - c.text_height(1) / 2), "DEV",
                color::kBg, 1, Align::Center);
}

void FpsTag::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    char buf[24];   // any two unsigneds, so -Wformat-truncation has nothing to prove
    std::snprintf(buf, sizeof buf, "%u/%u", static_cast<unsigned>(hz_ > 999 ? 999 : hz_),
                  static_cast<unsigned>(sent_ > 999 ? 999 : sent_));
    c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - c.text_height(1) / 2), buf,
                color::kAccent, 1, Align::Center);
}

}  // namespace ui

namespace ui {

char KeyGrid::key_char(int k) {
    static const char kKeys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";   // core::route::kAlphabet
    return k >= 2 && k < KeyGrid::kKeys ? kKeys[k - 2] : '\0';
}

void KeyGrid::set(uint64_t enabled, int cursor, const char* second) {
    assign_t(enabled_, enabled);
    assign_t(cursor_, cursor);
    if (std::strncmp(second_, second, sizeof second_) != 0) {
        std::snprintf(second_, sizeof second_, "%s", second);
        touch();
    }
}

void KeyGrid::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t w = static_cast<int16_t>(bounds.w / kCols);
    const int16_t h = static_cast<int16_t>(bounds.h / 3);
    for (int k = 0; k < kKeys; ++k) {
        const Rect cell{static_cast<int16_t>(bounds.x + (k % kCols) * w), static_cast<int16_t>(bounds.y + (k / kCols) * h),
                        static_cast<int16_t>(w - 2), static_cast<int16_t>(h - 2)};
        const bool on = (enabled_ >> k) & 1;
        const bool here = k == cursor_;
        if (here) c.fill_round_rect(cell, 3, color::kAccent);
        else if (on) c.draw_round_rect(cell, 3, theme::rule);
        const Color ink = here ? color::kBg : on ? color::kText : color::kGrid;
        char label[6];
        uint8_t scale = 2;
        if (k == 0) { std::snprintf(label, sizeof label, "DEL"); scale = 1; }
        else if (k == 1) { std::snprintf(label, sizeof label, "%s", second_); scale = 1; }
        else { label[0] = key_char(k) == ' ' ? '_' : key_char(k); label[1] = '\0'; }
        c.draw_text(cell.cx(), static_cast<int16_t>(cell.y + (cell.h - 7 * scale) / 2), label, ink, scale,
                    Align::Center);
    }
}

void TurnArrow::set(uint8_t kind, int16_t turn_deg, bool valid) {
    assign_t(kind_, kind);
    assign_t(turn_deg_, static_cast<int16_t>((turn_deg / 5) * 5));
    assign_t(valid_, valid);
}

namespace {
// A segment `w` px wide, as two triangles, with a round end at b.
void thick(Canvas& c, float ax, float ay, float bx, float by, float w, Color col) {
    const float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float px = -dy / len * w / 2, py = dx / len * w / 2;
    const auto r = [](float v) { return static_cast<int16_t>(std::lround(v)); };
    c.fill_triangle(r(ax + px), r(ay + py), r(bx + px), r(by + py), r(bx - px), r(by - py), col);
    c.fill_triangle(r(ax + px), r(ay + py), r(bx - px), r(by - py), r(ax - px), r(ay - py), col);
    c.fill_circle(r(bx), r(by), static_cast<int16_t>(w / 2), col);
}
// An arrowhead at (x, y), pointing along `a` radians (0 = up, + clockwise).
void head(Canvas& c, float x, float y, float a, float size, Color col) {
    const auto pt = [&](float ang, float len, int16_t& ox, int16_t& oy) {
        ox = static_cast<int16_t>(std::lround(x + std::sin(ang) * len));
        oy = static_cast<int16_t>(std::lround(y - std::cos(ang) * len));
    };
    int16_t tx, ty, lx, ly, rx, ry;
    pt(a, size, tx, ty);
    pt(a + 2.4f, size * 0.9f, lx, ly);
    pt(a - 2.4f, size * 0.9f, rx, ry);
    c.fill_triangle(tx, ty, lx, ly, rx, ry, col);
}
}  // namespace

void TurnArrow::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    if (!valid_) return;
    const float s = static_cast<float>(bounds.w < bounds.h ? bounds.w : bounds.h);
    const float cx = bounds.x + bounds.w / 2.0f;
    const float cy = bounds.y + bounds.h * 0.55f;
    const float w = s * 0.11f;
    const Color col = color::kAccent;
    const float bottom = bounds.y + bounds.h - w;
    // core::route::CueKind: 7 UTurn, 8 Arrive, 9 Roundabout.
    if (kind_ == 9) {
        // In from below, round the ring, out at the exit's own angle.
        const float rr = s * 0.17f;
        thick(c, cx, bottom, cx, cy + rr, w, col);
        c.fill_circle(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(rr + w / 2), col);
        c.fill_circle(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(rr - w / 2), color::kBg);
        const float a = turn_deg_ * 3.14159265f / 180.0f;
        const float sx = cx + std::sin(a) * rr, sy = cy - std::cos(a) * rr;
        const float ex = cx + std::sin(a) * s * 0.36f, ey = cy - std::cos(a) * s * 0.36f;
        thick(c, sx, sy, ex, ey, w, col);
        head(c, ex + std::sin(a) * s * 0.06f, ey - std::cos(a) * s * 0.06f, a, s * 0.2f, col);
        return;
    }
    if (kind_ == 8) {
        c.fill_circle(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(s * 0.30f), col);
        c.fill_circle(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(s * 0.20f), color::kBg);
        c.fill_circle(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(s * 0.10f), col);
        return;
    }
    if (kind_ == 7) {
        // Up the right, across, and back down the left: a U-turn in the US.
        const float xr = cx + s * 0.18f, xl = cx - s * 0.18f, top = bounds.y + s * 0.2f;
        thick(c, xr, bottom, xr, top, w, col);
        thick(c, xr, top, xl, top, w, col);
        thick(c, xl, top, xl, cy + s * 0.05f, w, col);
        head(c, xl, cy + s * 0.05f + s * 0.12f, 3.14159265f, s * 0.2f, col);
        return;
    }
    // The stem to the junction, then the arm at the turn's own angle.
    thick(c, cx, bottom, cx, cy, w, col);
    const float deg = turn_deg_ > 150 ? 150.0f : turn_deg_ < -150 ? -150.0f : static_cast<float>(turn_deg_);
    const float a = deg * 3.14159265f / 180.0f;
    const float arm = s * 0.30f;
    const float ex = cx + std::sin(a) * arm, ey = cy - std::cos(a) * arm;
    thick(c, cx, cy, ex, ey, w, col);
    head(c, ex + std::sin(a) * s * 0.06f, ey - std::cos(a) * s * 0.06f, a, s * 0.2f, col);
}

// --- QrCode -------------------------------------------------------------

void QrCode::set_text(const char* text) {
    if (!text_.set(text)) return;
    ok_ = !text_.empty() && core::qr::encode(text_.s, code_);
    touch();
}

void QrCode::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    if (!ok_) return;
    constexpr int kQuiet = 2;   // the spec's 4 is for print; a phone needs ~2
    const int16_t span = bounds.w < bounds.h ? bounds.w : bounds.h;
    const int16_t px = static_cast<int16_t>(span / (core::qr::kSize + 2 * kQuiet));
    if (px < 1) return;
    const int16_t side = static_cast<int16_t>(px * (core::qr::kSize + 2 * kQuiet));
    const int16_t x0 = static_cast<int16_t>(bounds.right() - side);   // right-aligned
    c.fill_rect(Rect{x0, bounds.y, side, side}, color::kWhite);
    const int16_t ox = static_cast<int16_t>(x0 + kQuiet * px);
    const int16_t oy = static_cast<int16_t>(bounds.y + kQuiet * px);
    for (int y = 0; y < core::qr::kSize; ++y) {
        // One rectangle per run of dark modules.
        for (int x = 0; x < core::qr::kSize;) {
            if (!code_.dark(x, y)) { ++x; continue; }
            int e = x;
            while (e < core::qr::kSize && code_.dark(e, y)) ++e;
            c.fill_rect(Rect{static_cast<int16_t>(ox + x * px), static_cast<int16_t>(oy + y * px),
                             static_cast<int16_t>((e - x) * px), px},
                        color::kBlack);
            x = e;
        }
    }
}

}  // namespace ui
