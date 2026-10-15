#include "ui/widgets.hpp"
#include "core/units/units.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include <cmath>
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
        // Peaks are records, so they take the half-degree step, not the hold.
        const float q = std::round(lean_deg * 2.0f) / 2.0f;
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
        const bool focused = static_cast<int>(r) == focus_;
        if (focused) {
            // Inside the bounds: on the first row the band starts above
            // them, and a partial repaint never clears what is out there.
            c.fill_rect(Rect{bounds.x, static_cast<int16_t>(y - 2), bounds.w,
                             static_cast<int16_t>(line)}.clipped_to(bounds),
                        color::kPanel);
        }
        c.draw_text(bounds.x, y, left_[r].s,
                    focused ? color::kWhite : color::kText, scale_, Align::Left);
        c.draw_text(bounds.right(), y, right_[r].s,
                    focused ? color::kAccent : color::kDim, scale_, Align::Right);
    }
}

// --- MapView ------------------------------------------------------------

void MapView::set_source(const IPointSource* src) { src_ = src; touch(); }

void MapView::set_live(bool valid) {
    assign_t(valid_, valid);
    // The trace is redrawn as it grows, every 10 points (1 s at 10 Hz), or
    // at once when it is cleared for a new lap. Nothing else moves it: the
    // GPS course used to dirty it on every fix, 79k px (16 ms of wire) for
    // a heading the map never drew.
    const size_t t = src_ ? src_->total() : 0;
    if (t < drawn_total_ || t >= drawn_total_ + kRedrawPoints) {
        drawn_total_ = t;
        touch();
    }
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

    float hx, hy;
    if (valid_ && src_->point(n - 1, hx, hy)) {
        const int16_t bx = static_cast<int16_t>(in.cx() + (hx - (min_x + max_x) / 2) * s);
        const int16_t by = static_cast<int16_t>(in.cy() - (hy - (min_y + max_y) / 2) * s);
        c.fill_circle(bx, by, 3, color::kWarn);
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

int TachBar::damage(Rect* out, int max) const {
    (void)max;
    if (whole_ || !moved_) { out[0] = bounds; return 1; }
    out[0] = Rect{bounds.x, moved_top_, bounds.w,
                  static_cast<int16_t>(moved_bottom_ - moved_top_)}.clipped_to(bounds);
    return out[0].empty() ? 0 : 1;
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
}

// --- LeanGauge ----------------------------------------------------------

void LeanGauge::set(float lean_deg, bool valid) {
    // Half a degree is finer than the arc can draw at this radius, and the
    // hold keeps noise from walking it back and forth.
    valid = usable(lean_deg, valid);
    const bool was_valid = valid_;
    const bool b = assign_t(valid_, valid);
    const bool a = valid && assign(lean_deg_, held_lean(lean_deg_, was_valid, lean_deg));
    if (a || b) touch();
}

void LeanGauge::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);

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

// --- GBall --------------------------------------------------------------

void GBall::set(float g_lat, float g_lon, bool valid) {
    // Quantised to 0.02 g, which is under a pixel of dot travel at this
    // radius. Without it the tile is dirty on every single frame the bike is
    // moving, and this is the largest widget on the screen — at 3 bytes a
    // pixel that is 60 kB down the SPI bus for a dot that has not visibly
    // gone anywhere.
    valid = valid && std::isfinite(g_lat) && std::isfinite(g_lon);
    const bool d = assign_t(valid_, valid);
    bool a = false, b = false;
    if (valid) {
        a = assign(g_lat_, std::round(g_lat * 50.0f) / 50.0f);
        b = assign(g_lon_, std::round(g_lon * 50.0f) / 50.0f);
    }
    if (a || b || d) touch();
}

void GBall::set_jump(uint32_t airtime_ms, float landing_g, bool show) {
    const bool a = assign_t(airtime_ms_, airtime_ms);
    const bool b = assign(landing_g_, landing_g);
    const bool d = assign_t(jump_, show);
    if (a || b || d) touch();
}

void GBall::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);

    const int16_t cx = bounds.cx();

    if (jump_) {
        // The jump face. Big enough to read from the air, which is the only
        // time it is ever up.
        c.draw_text(cx, static_cast<int16_t>(bounds.y + 10), "AIRTIME",
                    color::kDim, 1, Align::Center);

        char num[12];
        core::units::fmt_float(num, sizeof num,
                               static_cast<float>(airtime_ms_) / 1000.0f, 2);
        const int16_t nw = c.text_width(num, 5);
        const int16_t uw = c.text_width("s", 2);
        const int16_t x0 = static_cast<int16_t>(cx - (nw + 4 + uw) / 2);
        const int16_t ty = static_cast<int16_t>(bounds.y + 34);
        c.draw_text(x0, ty, num, color::kWhite, 5, Align::Left);
        c.draw_text(static_cast<int16_t>(x0 + nw + 4),
                    static_cast<int16_t>(ty + c.text_height(5) - c.text_height(2)),
                    "s", color::kDim, 2, Align::Left);

        char land[24];
        size_t k = core::units::fmt_float(land, sizeof land, landing_g_, 1);
        if (k + 3 < sizeof land) {
            land[k++] = ' '; land[k++] = 'g'; land[k] = '\0';
        }
        c.draw_text(cx, static_cast<int16_t>(bounds.y + 92), land,
                    landing_g_ >= 4.0f ? color::kWarn : color::kText, 3,
                    Align::Center);
        c.draw_text(cx, static_cast<int16_t>(bounds.y + 124), "LANDING",
                    color::kDim, 1, Align::Center);
        return;
    }

    const int16_t r  = static_cast<int16_t>(bounds.w / 2 - 14);
    const int16_t cy = static_cast<int16_t>(bounds.y + r + 5);

    // Two rings, at 1 g and at full scale, and a crosshair. The rider is not
    // reading a value off this — it is a shape, and the shape is the point.
    c.draw_circle(cx, cy, r, color::kGrid);
    c.draw_circle(cx, cy, static_cast<int16_t>(r / kFullScaleG), color::kGrid);
    c.draw_hline(static_cast<int16_t>(cx - r), cy,
                 static_cast<int16_t>(2 * r + 1), color::kGrid);
    c.draw_vline(cx, static_cast<int16_t>(cy - r),
                 static_cast<int16_t>(2 * r + 1), color::kGrid);

    float mag = std::sqrt(g_lat_ * g_lat_ + g_lon_ * g_lon_);

    if (valid_) {
        // Clamped to the rim rather than allowed off the tile: a dot that
        // leaves the circle would be drawn over the speed readout.
        float fx = g_lat_ / kFullScaleG;
        float fy = g_lon_ / kFullScaleG;
        const float f = std::sqrt(fx * fx + fy * fy);
        if (f > 1.0f) { fx /= f; fy /= f; }

        // Braking pushes the dot down, which is where the weight goes.
        const int16_t dx = static_cast<int16_t>(fx * r);
        const int16_t dy = static_cast<int16_t>(-fy * r);
        c.fill_circle(static_cast<int16_t>(cx + dx),
                      static_cast<int16_t>(cy + dy), 5, color::kAccent);
    }

    char buf[16];
    size_t k = core::units::fmt_value(buf, sizeof buf, mag, 2, valid_);
    if (k + 3 < sizeof buf) {
        buf[k++] = ' '; buf[k++] = 'g'; buf[k] = '\0';
    }
    c.draw_text(cx, static_cast<int16_t>(cy + r + 8), buf,
                valid_ ? color::kText : color::kDim, 2, Align::Center);
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

}  // namespace ui

namespace ui {

void NavArrow::set(float rel_deg, bool heading_up, bool valid) {
    // Two-degree steps: a parked GPS wanders, and an arrow that redraws on
    // every fix for a change nobody could see costs a blit each time.
    valid = usable(rel_deg, valid);     // lround(NaN) is undefined
    const int16_t q = valid ? static_cast<int16_t>(std::lround(rel_deg / 2.0f) * 2) : 0;
    assign_t(rel_deg_, q);
    assign_t(heading_up_, heading_up);
    assign_t(valid_, valid);
}

void NavArrow::render(Canvas& c) {
    c.fill_rect(bounds, color::kBg);
    const int16_t cx = bounds.cx();
    const int16_t cy = static_cast<int16_t>(bounds.y + bounds.h / 2);
    const int16_t r = static_cast<int16_t>((bounds.w < bounds.h ? bounds.w : bounds.h) / 2 - 2);
    c.draw_circle(cx, cy, r, theme::rule);
    c.draw_circle(cx, cy, static_cast<int16_t>(r - 1), theme::rule);
    // What "up" means, at the top of the ring.
    c.draw_text(cx, static_cast<int16_t>(cy - r + 6), heading_up_ ? "^" : "N",
                theme::secondary, 2, Align::Center);
    if (!valid_) {
        c.draw_text(cx, static_cast<int16_t>(cy - 14), "--", color::kDim, 4, Align::Center);
        return;
    }
    const float a = static_cast<float>(rel_deg_) * kPi / 180.0f;
    const auto pt = [&](float ang, float len, int16_t& x, int16_t& y) {
        x = static_cast<int16_t>(cx + std::lround(std::sin(ang) * len));
        y = static_cast<int16_t>(cy - std::lround(std::cos(ang) * len));
    };
    // A chevron with a notch: the tip, two barbs, and a point behind the
    // centre the two halves share, so the shape reads at a glance.
    int16_t tx, ty, lx, ly, rx, ry, nx, ny;
    pt(a, r * 0.72f, tx, ty);
    pt(a + 2.5f, r * 0.56f, lx, ly);
    pt(a - 2.5f, r * 0.56f, rx, ry);
    pt(a + kPi, r * 0.22f, nx, ny);
    c.fill_triangle(tx, ty, lx, ly, nx, ny, color::kAccent);
    c.fill_triangle(tx, ty, nx, ny, rx, ry, color::kAccent);
}

}  // namespace ui
