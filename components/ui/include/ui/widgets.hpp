// The widget set (§7.4).
//
// Every one of these that shows a sensor reading takes a `valid` flag beside
// the number and draws `--` when it is false. That is the §12 rule made
// unavoidable: a dead sensor and a genuine zero must never look the same on
// a dashboard, because one of them means stop riding.
#pragma once
#include <cstdint>
#include "ui/widget.hpp"
#include "core/state/vehicle.hpp"

namespace ui {

// Fixed-size text storage. Nothing here allocates after boot.
template <size_t N>
struct TextBuf {
    char s[N] = {};
    bool set(const char* v);          // true if it changed
    bool empty() const { return s[0] == '\0'; }
};

// A caption. Static text, drawn once, redrawn when the screen is entered.
class Label : public Widget {
public:
    void configure(const char* text, Color c = color::kDim,
                   uint8_t scale = 1, Align align = Align::Left);
    void set_text(const char* text);
    void render(Canvas& c) override;

private:
    TextBuf<24> text_;
    Color       color_ = color::kDim;
    uint8_t     scale_ = 1;
    Align       align_ = Align::Left;
};

// A caption over a number with a unit. The workhorse of the stat screens.
class ValueBox : public Widget {
public:
    void configure(const char* label, const char* unit, int decimals = 1,
                   uint8_t scale = 2);
    void set(float value, bool valid = true);
    void set_unit(const char* unit);
    void render(Canvas& c) override;

private:
    TextBuf<16> label_;
    TextBuf<8>  unit_;
    float       value_ = 0.0f;
    bool        valid_ = false;
    int         decimals_ = 1;
    uint8_t     scale_ = 2;
};

// The one number a rider actually looks at.
class BigNumber : public Widget {
public:
    void configure(const char* unit, int decimals = 0, uint8_t scale = 8);
    void set(float value, bool valid = true);
    void set_color(Color c);
    void render(Canvas& c) override;

private:
    TextBuf<8> unit_;
    float      value_ = 0.0f;
    bool       valid_ = false;
    int        decimals_ = 0;
    uint8_t    scale_ = 8;
    Color      color_ = color::kText;
};

// A horizontal bar, 0..1. Coolant, fuel, throttle.
class Bar : public Widget {
public:
    void configure(Color fill, Color warn_fill, float warn_above = 2.0f);
    void set(float fraction, bool valid = true);
    void render(Canvas& c) override;

private:
    float fraction_ = 0.0f;
    bool  valid_ = false;
    Color fill_ = color::kAccent;
    Color warn_ = color::kAlarm;
    float warn_above_ = 2.0f;
};

// One device's health, as a coloured dot with a name. The DIAG sensors
// screen is a column of these, and it is the bring-up tool when the PCB
// arrives.
class StatusDot : public Widget {
public:
    void configure(const char* name);
    void set(core::state::DevState s);
    void render(Canvas& c) override;

private:
    TextBuf<12>           name_;
    core::state::DevState state_ = core::state::DevState::Absent;
};

// Eight lamps, driven straight off core/shift's mask so the screen and the
// real lamps can never disagree.
class ShiftStrip : public Widget {
public:
    void set(uint8_t mask, bool flash);
    void render(Canvas& c) override;

private:
    uint8_t mask_ = 0;
    bool    flash_ = false;
};

// Lap delta: a bar either side of centre plus the number. Ahead is green and
// to the left, behind is red and to the right, and the rider reads the
// colour long before the digits.
class DeltaBar : public Widget {
public:
    void set(int32_t delta_ms, bool valid);
    void render(Canvas& c) override;

private:
    int32_t delta_ms_ = 0;
    bool    valid_ = false;
};

// Lean angle, drawn as a tilting horizon with the peak either side held.
class LeanIndicator : public Widget {
public:
    void set(float lean_deg, bool valid);
    void reset_peaks();
    void render(Canvas& c) override;

private:
    float lean_deg_ = 0.0f;
    float peak_left_ = 0.0f;
    float peak_right_ = 0.0f;
    bool  valid_ = false;
};

// A rolling trace. Fixed capacity, no allocation; push() is O(1).
class Sparkline : public Widget {
public:
    static constexpr size_t kPoints = 64;
    void configure(float min, float max, Color c = color::kAccent);
    void push(float v);
    void clear();
    void render(Canvas& c) override;

private:
    float  points_[kPoints] = {};
    size_t count_ = 0;
    size_t head_ = 0;
    float  min_ = 0.0f;
    float  max_ = 1.0f;
    Color  color_ = color::kAccent;
};

// A two-column table. The lap history and the config list are both this.
class Table : public Widget {
public:
    static constexpr size_t kRows = 10;
    void configure(uint8_t visible_rows, uint8_t scale = 1);
    void set_row(size_t i, const char* left, const char* right);
    void set_row_count(size_t n);
    void set_focus(int focus);          // -1 for none
    void clear_rows();
    size_t row_count() const { return rows_; }
    void render(Canvas& c) override;

private:
    TextBuf<20> left_[kRows];
    TextBuf<14> right_[kRows];
    size_t      rows_ = 0;
    int         focus_ = -1;
    uint8_t     visible_ = kRows;
    uint8_t     scale_ = 1;
};

// The track and trip maps read their points through this rather than owning
// them: the points live in a TrackBuffer (§4.4) with one writer, and copying
// 4000 of them into a widget every frame is the thing that buffer exists to
// avoid.
class IPointSource {
public:
    virtual ~IPointSource() = default;
    virtual size_t count() const = 0;
    virtual bool   point(size_t i, float& x_ft, float& y_ft) const = 0;
};

class MapView : public Widget {
public:
    void set_source(const IPointSource* src);
    void set_position(float x_ft, float y_ft, float heading_deg, bool valid);
    void render(Canvas& c) override;

private:
    const IPointSource* src_ = nullptr;
    float x_ft_ = 0.0f, y_ft_ = 0.0f, heading_deg_ = 0.0f;
    bool  valid_ = false;
};

// One leg of the tach bar: the L that runs up the left edge of the speed
// screen and along the top.
//
// It is a widget rather than something the screen paints because it changes
// with rpm, and only widgets are tracked for redraw — chrome painted by a
// screen is refreshed on a full repaint and never in between, which on a
// partial-update display means it would simply stop moving.
class TachSegment : public Widget {
public:
    enum class Fill : uint8_t { Up = 0, Right };

    // `from` and `to` are this leg's share of the whole bar's travel, so the
    // two legs together cover 0..1 and the fill crosses the corner smoothly.
    void configure(Fill dir, float from, float to, bool cut_diagonal);
    void set(float fraction);      // 0..1 of the whole bar
    void render(Canvas& c) override;

private:
    Fill  dir_ = Fill::Up;
    float from_ = 0.0f, to_ = 1.0f;
    bool  cut_ = false;
    float fraction_ = 0.0f;
};

// The lean gauge on the speed screen: a half annulus with a marker on it and
// the angle in the middle.
class LeanGauge : public Widget {
public:
    void set(float lean_deg, bool valid);
    void render(Canvas& c) override;

private:
    float lean_deg_ = 0.0f;
    bool  valid_ = false;
};

// The accelerometer tile: lateral against longitudinal g as a dot in a
// friction circle, and the last jump instead of it for a few seconds after a
// landing.
//
// One widget with two faces rather than two widgets swapped in and out of a
// screen's list, because the rectangle is the unit of redraw: two widgets
// sharing a rectangle would each leave the other's pixels behind on a
// partial update, which is exactly the invariant test_ui exists to hold.
class GBall : public Widget {
public:
    // Both in g, from the IMU: `g_lat` positive to the right, `g_lon`
    // positive under acceleration. These are what core/attitude publishes;
    // `motion.accel_g` is nobody's to write on this board.
    void set(float g_lat, float g_lon, bool valid);

    // `show` latches the jump face on. How long that lasts is the screen's
    // business — a widget has no clock, and a Frame has no history.
    void set_jump(uint32_t airtime_ms, float landing_g, bool show);

    void render(Canvas& c) override;

    // Full scale. A CRF250F on dirt will not see much past 1 g either way,
    // and a circle scaled for a superbike leaves the dot sitting in the
    // middle doing nothing.
    static constexpr float kFullScaleG = 1.5f;

private:
    float    g_lat_ = 0.0f;
    float    g_lon_ = 0.0f;
    bool     valid_ = false;
    uint32_t airtime_ms_ = 0;
    float    landing_g_ = 0.0f;
    bool     jump_ = false;
};

// What is playing: song, album and artist, each behind its icon.
//
// One widget rather than three labels and three background icons, because
// the icons must disappear along with the text. Chrome painted by a screen
// is only refreshed on a full repaint, so three icons drawn as background
// would still be sitting there next to nothing when the music stopped.
//
// The text is truncated to the width available. A marquee would fit more,
// but it would repaint this block on every frame forever, and a scrolling
// album title is not worth a permanent share of the SPI budget.
class MediaBlock : public Widget {
public:
    // `valid` is false when nothing has ever arrived, or when the phone has
    // gone away — a dash still showing what was playing forty minutes ago is
    // worse than one showing nothing.
    void set(const char* title, const char* album, const char* artist,
             bool valid);
    void render(Canvas& c) override;

    static constexpr int16_t kRowH = 22;

private:
    TextBuf<40> title_;
    TextBuf<40> album_;
    TextBuf<40> artist_;
    bool        valid_ = false;
};

// The one widget allowed to draw over whatever screen is up. A tyre losing
// air is worth interrupting the trip map for (§7.4).
class AlertOverlay : public Widget {
public:
    void show(const char* text, Color c = color::kAlarm);
    void dismiss();
    bool active() const { return active_; }
    void render(Canvas& c) override;

private:
    TextBuf<28> text_;
    Color       color_ = color::kAlarm;
    bool        active_ = false;
};

}  // namespace ui
