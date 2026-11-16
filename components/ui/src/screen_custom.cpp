// Custom screens: what the rider drew on the hotspot page, from the deck.
#include "ui/screens.hpp"
#include "core/config/config.hpp"
#include "ui/icons.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {

namespace lay = core::layout;

namespace {
// A gauge's sweep: 240 degrees, from south-west round to south-east,
// clockwise from north as Canvas::fill_arc counts.
constexpr float kGaugeStart = 240.0f;
constexpr float kGaugeSweep = 240.0f;
constexpr uint32_t kTraceStepMs = 100;

const icon::Bitmap* builtin_icon(uint8_t i) {
    switch (i) {
        case 0: return &icon::kSong;
        case 1: return &icon::kAlbum;
        case 2: return &icon::kArtist;
        case 3: return &icon::kLyric;
        default: return nullptr;
    }
}

// Clip `s` in place to what fits `w` pixels at `scale`.
void fit(char* s, int16_t w, uint8_t scale) {
    const int per = 6 * scale;
    const int max = per > 0 ? (w + scale) / per : 0;
    if (max >= 0 && static_cast<int>(std::strlen(s)) > max) s[max] = '\0';
}

float fraction(const lay::Readout& r, double v) {
    const float f = static_cast<float>((v - r.lo) / (r.hi - r.lo));
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}
}  // namespace

// --- ReadoutWidget ------------------------------------------------------------

void ReadoutWidget::configure(const lay::Readout* r, uint8_t* trace) {
    r_ = r;
    f_ = r ? lay::find_field(r->field) : nullptr;
    trace_ = trace;
    trace_n_ = 0;
    trace_head_ = 0;
    trace_next_ms_ = 0;
    if (r) set_bounds(Rect{r->x, r->y, r->w, r->h});
    touch();
}

void ReadoutWidget::update(const core::state::Frame& fr, const lay::ShowCtx& c) {
    if (!r_) return;
    if (!f_) {   // a field this firmware does not have
        if (text_.set("?")) touch();
        return;
    }
    const lay::Shown s = lay::show(*f_, fr, c, r_->decimals);
    bool changed = text_.set(s.text);
    changed |= assign_t(unit_, s.unit);
    changed |= assign_t(valid_, s.valid);

    // Thresholds in the native unit, like the range.
    const bool below = r_->flags & lay::Readout::kBelow;
    auto past = [&](float t) { return below ? s.number <= t : s.number >= t; };
    Color ink = r_->ink;
    if ((r_->flags & lay::Readout::kAlarm) && s.valid && past(r_->alarm)) ink = color::kAlarm;
    else if ((r_->flags & lay::Readout::kWarn) && s.valid && past(r_->warn)) ink = color::kWarn;
    changed |= assign_t(ink_, ink);

    const bool lit = s.valid && ((r_->flags & (lay::Readout::kWarn | lay::Readout::kAlarm))
                                     ? ink != r_->ink : s.number != 0.0);
    changed |= assign_t(lit_, lit);

    if (r_->style == lay::Style::Bar || r_->style == lay::Style::Gauge) {
        const int16_t span = r_->style == lay::Style::Bar ? r_->w : 240;
        const int16_t px = s.valid ? static_cast<int16_t>(fraction(*r_, s.number) * span) : -1;
        changed |= assign_t(fill_px_, px);
    }
    if (r_->style == lay::Style::Spark && trace_ && fr.t_ms >= trace_next_ms_) {
        trace_next_ms_ = fr.t_ms + kTraceStepMs;
        trace_[trace_head_] = s.valid ? static_cast<uint8_t>(fraction(*r_, s.number) * 254.0f + 0.5f)
                                      : kNoSample;
        trace_head_ = static_cast<uint8_t>((trace_head_ + 1) % kTraceLen);
        if (trace_n_ < kTraceLen) ++trace_n_;
        changed = true;
    }
    if (changed) touch();
}

void ReadoutWidget::render(Canvas& c) {
    c.fill_rect(bounds, r_ ? r_->ground : color::kBg);
    if (!r_) return;
    const lay::Readout& r = *r_;
    const char* cap = r.caption[0] ? r.caption : f_ ? f_->caption : "?";
    const bool cap_on = r.flags & lay::Readout::kCaption;
    const bool unit_on = (r.flags & lay::Readout::kUnit) && unit_ && unit_[0];
    const Color ink = valid_ ? ink_ : color::kDim;
    const Align al = r.align == 1 ? Align::Center : r.align == 2 ? Align::Right : Align::Left;
    const int16_t ax = al == Align::Center ? bounds.cx() : al == Align::Right ? bounds.right() : bounds.x;
    char t[32];
    std::snprintf(t, sizeof t, "%s", text_.s);

    switch (r.style) {
        case lay::Style::Value: {
            int16_t y = bounds.y;
            if (cap_on) {
                char cp[lay::kCaptionLen];
                std::snprintf(cp, sizeof cp, "%s", cap);
                fit(cp, bounds.w, 1);
                c.draw_text(ax, y, cp, color::kDim, 1, al);
                y = static_cast<int16_t>(y + c.text_height(1) + 4);
            }
            const int16_t uw = unit_on ? static_cast<int16_t>(c.text_width(unit_, 1) + 6) : 0;
            fit(t, static_cast<int16_t>(bounds.w - uw), r.scale);
            int16_t vx = ax;
            const int16_t tw = c.text_width(t, r.scale);
            // The value and its unit are one line, aligned as a whole.
            if (al == Align::Center) vx = static_cast<int16_t>(bounds.cx() - (tw + uw) / 2);
            else if (al == Align::Right) vx = static_cast<int16_t>(bounds.right() - tw - uw);
            c.draw_text(vx, y, t, ink, r.scale, Align::Left);
            if (unit_on) {
                c.draw_text(static_cast<int16_t>(vx + tw + 6),
                            static_cast<int16_t>(y + c.text_height(r.scale) - c.text_height(1)),
                            unit_, color::kDim, 1, Align::Left);
            }
            return;
        }
        case lay::Style::Big: {
            fit(t, bounds.w, r.scale);
            const int16_t th = c.text_height(r.scale);
            const bool under = unit_on && bounds.h >= th + 6 + c.text_height(2);
            const int16_t block = static_cast<int16_t>(th + (under ? 6 + c.text_height(2) : 0));
            const int16_t y = static_cast<int16_t>(bounds.cy() - block / 2);
            c.draw_text(bounds.cx(), y, t, ink, r.scale, Align::Center);
            if (under) c.draw_text(bounds.cx(), static_cast<int16_t>(y + th + 6), unit_, color::kDim, 2, Align::Center);
            return;
        }
        case lay::Style::Text: {
            fit(t, bounds.w, r.scale);
            const int16_t y = static_cast<int16_t>(bounds.cy() - c.text_height(r.scale) / 2);
            c.draw_text(ax, y, t, ink, r.scale, al);
            return;
        }
        case lay::Style::Bar: {
            c.fill_rect(bounds, color::kPanel);
            c.draw_rect(bounds, color::kGrid);
            if (fill_px_ < 0) {
                c.draw_text(bounds.cx(), static_cast<int16_t>(bounds.cy() - 3), "--", color::kDim, 1, Align::Center);
                return;
            }
            const Rect in = bounds.inset(2);
            const int16_t w = static_cast<int16_t>(static_cast<int32_t>(in.w) * fill_px_ / (bounds.w ? bounds.w : 1));
            if (w > 0) c.fill_rect(Rect{in.x, in.y, w, in.h}, ink_);
            return;
        }
        case lay::Style::Gauge: {
            const int16_t rad = static_cast<int16_t>((bounds.w < bounds.h ? bounds.w : bounds.h) / 2);
            const int16_t th = static_cast<int16_t>(rad / 5 > 2 ? rad / 5 : 2);
            const int16_t cx = bounds.cx(), cy = bounds.cy();
            c.fill_arc(cx, cy, rad, static_cast<int16_t>(rad - th), kGaugeStart,
                       kGaugeStart + kGaugeSweep, color::kGrid);
            if (fill_px_ > 0) {
                c.fill_arc(cx, cy, rad, static_cast<int16_t>(rad - th), kGaugeStart,
                           kGaugeStart + kGaugeSweep * fill_px_ / 240.0f, ink_);
            }
            fit(t, static_cast<int16_t>(2 * (rad - th) - 4), r.scale);
            const int16_t h = c.text_height(r.scale);
            c.draw_text(cx, static_cast<int16_t>(cy - h / 2), t, ink, r.scale, Align::Center);
            if (cap_on) {
                char cp[lay::kCaptionLen];
                std::snprintf(cp, sizeof cp, "%s", cap);
                fit(cp, static_cast<int16_t>(2 * (rad - th)), 1);
                c.draw_text(cx, static_cast<int16_t>(cy + h / 2 + 4), cp, color::kDim, 1, Align::Center);
            }
            if (unit_on) c.draw_text(cx, static_cast<int16_t>(cy - h / 2 - c.text_height(1) - 4), unit_,
                                     color::kDim, 1, Align::Center);
            return;
        }
        case lay::Style::Lamp: {
            const int16_t d = bounds.h < bounds.w ? bounds.h : bounds.w;
            const int16_t rad = static_cast<int16_t>(d / 2);
            const int16_t cx = static_cast<int16_t>(bounds.x + rad);
            const int16_t cy = bounds.cy();
            c.fill_circle(cx, cy, rad, lit_ ? ink_ : color::kPanel);
            c.draw_circle(cx, cy, rad, color::kGrid);
            if (cap_on && bounds.w > d + 8) {
                char cp[lay::kCaptionLen];
                std::snprintf(cp, sizeof cp, "%s", cap);
                fit(cp, static_cast<int16_t>(bounds.w - d - 6), 1);
                c.draw_text(static_cast<int16_t>(bounds.x + d + 6),
                            static_cast<int16_t>(cy - c.text_height(1) / 2), cp,
                            lit_ ? color::kText : color::kDim, 1, Align::Left);
            }
            return;
        }
        case lay::Style::Spark: {
            c.fill_rect(bounds, color::kPanel);
            if (!trace_ || trace_n_ < 2) return;
            const int n = trace_n_ < bounds.w ? trace_n_ : bounds.w;
            const int16_t top = static_cast<int16_t>(bounds.y + 1);
            const int16_t hgt = static_cast<int16_t>(bounds.h - 3);
            int16_t px = -1, py = -1;
            for (int i = 0; i < n; ++i) {
                // Newest at the right edge.
                const int k = (trace_head_ - n + i + kTraceLen) % kTraceLen;
                const uint8_t sample = trace_[k];
                const int16_t x = static_cast<int16_t>(bounds.right() - 1 - (n - 1 - i) * (bounds.w - 1) / (n - 1));
                if (sample == kNoSample) { px = -1; continue; }
                const int16_t y = static_cast<int16_t>(top + hgt - sample * hgt / 254);
                if (px >= 0) c.draw_line(px, py, x, y, ink_);
                px = x;
                py = y;
            }
            return;
        }
        case lay::Style::kCount:
            return;
    }
}

// --- CustomScreen ----------------------------------------------------------------

void CustomScreen::set_deck(const lay::Deck* d, uint8_t index) {
    deck_ = d;
    index_ = index;
    screen_ = d && index < d->screen_count ? &d->screens[index] : nullptr;
}

const char* CustomScreen::name() const { return screen_ ? screen_->name : "CUSTOM"; }

bool CustomScreen::wants_title() const {
    return !screen_ || (screen_->flags & lay::Screen::kTitle);
}

void CustomScreen::enter(Canvas&) {
    n_ = 0;
    armed_ = false;
    if (!screen_) return;
    int spark = 0;
    for (int i = 0; i < screen_->readout_n && n_ < lay::kMaxReadoutsPerScreen; ++i) {
        const lay::Readout* r = &deck_->readouts[screen_->readout_first + i];
        uint8_t* trace = nullptr;
        if (r->style == lay::Style::Spark && spark < lay::kMaxSparksPerScreen) trace = traces_[spark++];
        ro_[n_].configure(r, trace);
        list_[n_] = &ro_[n_];
        ++n_;
    }
}

void CustomScreen::update(const core::state::Frame& f) {
    lay::ShowCtx c;
    c.units = units();
    if (has_ctx() && ctx().settings) {
        c.tz_offset_h = ctx().settings->tz_offset_h;
        c.clock_12h = ctx().settings->clock_12h;
    }
    for (uint8_t i = 0; i < n_; ++i) ro_[i].update(f, c);
}

void CustomScreen::draw_background(Canvas& c) {
    if (!screen_) {
        c.draw_text(kScreenW / 2, 150, "NO SUCH SCREEN", color::kDim, 2, Align::Center);
        return;
    }
    const int16_t top = wants_title() ? lay::kTitleH : 0;
    c.fill_rect(Rect{0, top, kScreenW, static_cast<int16_t>(kScreenH - top)}, screen_->background);
    for (int i = 0; i < screen_->chrome_n; ++i) {
        const lay::Chrome& e = deck_->chrome[screen_->chrome_first + i];
        const Rect box{e.x, e.y, e.w, e.h};
        const int16_t rad = static_cast<int16_t>((e.w < e.h ? e.w : e.h) / 2);
        switch (e.shape) {
            case lay::Shape::Line:
                c.draw_line(e.x, e.y, static_cast<int16_t>(e.x + e.w), static_cast<int16_t>(e.y + e.h), e.color);
                break;
            case lay::Shape::Rect:          c.draw_rect(box, e.color); break;
            case lay::Shape::FillRect:      c.fill_rect(box, e.color); break;
            case lay::Shape::RoundRect:     c.draw_round_rect(box, e.radius, e.color); break;
            case lay::Shape::FillRoundRect: c.fill_round_rect(box, e.radius, e.color); break;
            case lay::Shape::Circle:        c.draw_circle(box.cx(), box.cy(), rad, e.color); break;
            case lay::Shape::Disc:          c.fill_circle(box.cx(), box.cy(), rad, e.color); break;
            case lay::Shape::Arc: {
                float a0 = e.a0, a1 = e.a1;
                while (a0 < 0.0f) { a0 += 360.0f; a1 += 360.0f; }
                const int16_t inner = static_cast<int16_t>(rad - (e.radius ? e.radius : 1));
                c.fill_arc(box.cx(), box.cy(), rad, inner > 0 ? inner : 0, a0, a1, e.color);
                break;
            }
            case lay::Shape::Triangle:
                c.fill_triangle(e.x, e.y, static_cast<int16_t>(e.x + e.w), static_cast<int16_t>(e.y + e.h),
                                e.x3, e.y3, e.color);
                break;
            case lay::Shape::Text: {
                const Align al = e.align == 1 ? Align::Center : e.align == 2 ? Align::Right : Align::Left;
                c.draw_text(e.x, e.y, e.text, e.color, e.scale, al);
                break;
            }
            case lay::Shape::Icon:
                if (e.icon >= lay::kUserImage) {
                    const lay::Image& im = deck_->images[e.icon - lay::kUserImage];
                    c.draw_bitmap(e.x, e.y, deck_->image_bits + im.offset, im.w, im.h, e.color);
                } else if (const icon::Bitmap* b = builtin_icon(e.icon)) {
                    c.draw_bitmap(e.x, e.y, b->bits, b->w, b->h, e.color);
                }
                break;
            case lay::Shape::kCount:
                break;
        }
    }
}

bool CustomScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    const bool short_e = e.button == Button::E && e.press == Press::Short;
    if (!short_e) {
        armed_ = false;   // any other button cancels a half-confirmed action
        return false;
    }
    const core::config::ConfigItem* it =
        screen_ && screen_->e_action ? core::config::find(screen_->e_action) : nullptr;
    if (!it || it->kind != core::config::Kind::Action) return false;
    if (it->confirm && !armed_) {
        armed_ = true;
        std::snprintf(prompt_, sizeof prompt_, "E AGAIN: %s", it->label);
        for (char* p = prompt_; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
        }
        return true;
    }
    armed_ = false;
    post(core::state::Command(it->action));
    return true;
}

WidgetList CustomScreen::widgets() { return {list_, n_}; }

const char* CustomScreen::widget_name(size_t i) const {
    if (i >= n_ || !ro_[i].bounds.w) return nullptr;
    const lay::Readout& r = deck_->readouts[screen_->readout_first + i];
    const lay::FieldDef* f = lay::find_field(r.field);
    return f ? f->path : nullptr;
}

}  // namespace ui
