#include "ui/widgets.hpp"
#include "core/units/units.hpp"
#include <cmath>

namespace ui {

namespace {
constexpr float kPi = 3.14159265358979f;

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
    if (text_.set(text)) touch();
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
    // Compare at display resolution. A coolant sensor jittering in the
    // fourth decimal must not repaint the widget 30 times a second.
    float scale = 1.0f;
    for (int i = 0; i < decimals_; ++i) scale *= 10.0f;
    const float q = std::round(value * scale) / scale;
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
    const float q = std::round(value * scale) / scale;
    const bool changed = assign_t(valid_, valid) | (valid && assign(value_, q));
    if (changed) touch();
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
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    // A pixel of bar is worth repainting for; a thousandth is not.
    const float q = std::round(fraction * 100.0f) / 100.0f;
    const bool changed = assign_t(valid_, valid) | assign(fraction_, q);
    if (changed) touch();
}

void Bar::render(Canvas& c) {
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
        // Green, amber, red across the strip: the same order as the lamps on
        // the bar, so the screen and the bike agree at a glance.
        Color c_on = color::kGood;
        if (i >= 5) c_on = color::kAlarm;
        else if (i >= 3) c_on = color::kWarn;
        const Rect r{static_cast<int16_t>(bounds.x + i * (w + gap)), bounds.y,
                     w, bounds.h};
        c.fill_rect(r, lit ? c_on : color::kPanel);
    }
}

// --- DeltaBar -----------------------------------------------------------

void DeltaBar::set(int32_t delta_ms, bool valid) {
    const bool a = assign_t(delta_ms_, delta_ms);
    const bool b = assign_t(valid_, valid);
    if (a || b) touch();
}

void DeltaBar::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t mid = bounds.cx();
    c.draw_vline(mid, bounds.y, bounds.h, color::kGrid);

    char buf[16];
    core::units::fmt_delta(buf, sizeof buf, delta_ms_, valid_);
    if (!valid_) {
        c.draw_text(mid, bounds.y, buf, color::kDim, 2, Align::Center);
        return;
    }

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
    const float q = std::round(lean_deg * 2.0f) / 2.0f;
    const bool a = assign(lean_deg_, q);
    const bool b = assign_t(valid_, valid);
    if (valid) {
        if (q < peak_left_)  { peak_left_ = q;  touch(); }
        if (q > peak_right_) { peak_right_ = q; touch(); }
    }
    if (a || b) touch();
}

void LeanIndicator::reset_peaks() {
    peak_left_ = 0.0f;
    peak_right_ = 0.0f;
    touch();
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
    if (i + 1 > rows_) rows_ = i + 1;
    if (a || b) touch();
}

void Table::set_row_count(size_t n) {
    if (n > kRows) n = kRows;
    if (rows_ != n) { rows_ = n; touch(); }
}

void Table::clear_rows() {
    for (size_t i = 0; i < kRows; ++i) { left_[i].set(""); right_[i].set(""); }
    rows_ = 0;
    touch();
}

void Table::set_focus(int focus) { assign_t(focus_, focus); }

void Table::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t line = static_cast<int16_t>(c.text_height(scale_) + 6);

    // Scroll so the focused row stays on screen. Without this the config
    // list simply stops responding once the cursor walks past the bottom.
    size_t first = 0;
    if (focus_ >= 0 && static_cast<size_t>(focus_) >= visible_) {
        first = static_cast<size_t>(focus_) - visible_ + 1;
    }

    for (size_t i = 0; i < visible_ && first + i < rows_; ++i) {
        const size_t r = first + i;
        const int16_t y = static_cast<int16_t>(bounds.y + i * line);
        const bool focused = static_cast<int>(r) == focus_;
        if (focused) {
            c.fill_rect(Rect{bounds.x, static_cast<int16_t>(y - 2), bounds.w,
                             static_cast<int16_t>(line)}, color::kPanel);
        }
        c.draw_text(bounds.x, y, left_[r].s,
                    focused ? color::kWhite : color::kText, scale_, Align::Left);
        c.draw_text(bounds.right(), y, right_[r].s,
                    focused ? color::kAccent : color::kDim, scale_, Align::Right);
    }
}

// --- MapView ------------------------------------------------------------

void MapView::set_source(const IPointSource* src) { src_ = src; touch(); }

void MapView::set_position(float x_ft, float y_ft, float heading_deg, bool valid) {
    const bool a = assign(x_ft_, x_ft);
    const bool b = assign(y_ft_, y_ft);
    const bool d = assign(heading_deg_, heading_deg);
    const bool e = assign_t(valid_, valid);
    if (a || b || d || e) touch();
}

void MapView::render(Canvas& c) {
    c.fill_rect(bounds, color::kPanel);
    c.draw_rect(bounds, color::kGrid);

    const size_t n = src_ ? src_->count() : 0;
    if (n < 2) {
        c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - 3),
                    "no trace", color::kDim, 1, Align::Center);
        return;
    }

    // Fit the whole trace, walking it twice rather than keeping a copy. The
    // buffer is the owner of these points (§4.4) and this is a reader.
    float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        float x, y;
        if (!src_->point(i, x, y)) continue;
        if (first) { min_x = max_x = x; min_y = max_y = y; first = false; continue; }
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    if (first) return;

    const float span_x = (max_x - min_x) > 1.0f ? (max_x - min_x) : 1.0f;
    const float span_y = (max_y - min_y) > 1.0f ? (max_y - min_y) : 1.0f;
    const Rect in = bounds.inset(6);
    const float sx = in.w / span_x;
    const float sy = in.h / span_y;
    const float s = sx < sy ? sx : sy;          // square, or the track lies

    int16_t px = 0, py = 0;
    bool have_prev = false;
    for (size_t i = 0; i < n; ++i) {
        float x, y;
        if (!src_->point(i, x, y)) { have_prev = false; continue; }
        const int16_t dx = static_cast<int16_t>(in.cx() + (x - (min_x + max_x) / 2) * s);
        // Screen y grows downwards; north does not.
        const int16_t dy = static_cast<int16_t>(in.cy() - (y - (min_y + max_y) / 2) * s);
        if (have_prev) c.draw_line(px, py, dx, dy, color::kAccent);
        px = dx;
        py = dy;
        have_prev = true;
    }

    if (valid_) {
        const int16_t bx = static_cast<int16_t>(in.cx() + (x_ft_ - (min_x + max_x) / 2) * s);
        const int16_t by = static_cast<int16_t>(in.cy() - (y_ft_ - (min_y + max_y) / 2) * s);
        c.fill_circle(bx, by, 3, color::kWarn);
    }
}

// --- AlertOverlay -------------------------------------------------------

void AlertOverlay::show(const char* text, Color c) {
    const bool a = text_.set(text);
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
                "E to dismiss", color::kDim, 1, Align::Center);
}

}  // namespace ui
