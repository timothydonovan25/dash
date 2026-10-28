// The widget set (§7.4).
//
// Every one of these that shows a sensor reading takes a `valid` flag beside
// the number and draws `--` when it is false. That is the §12 rule made
// unavoidable: a dead sensor and a genuine zero must never look the same on
// a dashboard, because one of them means stop riding.
#pragma once
#include <cstdint>
#include "core/hr/hr.hpp"
#include "core/slip/slip.hpp"
#include "ui/widget.hpp"
#include "ui/album_art.hpp"
#include "core/state/vehicle.hpp"

namespace ui {

// Fixed-size text storage. Nothing here allocates after boot.
template <size_t N>
struct TextBuf {
    char s[N] = {};
    bool set(const char* v);          // true if it changed
    bool empty() const { return s[0] == '\0'; }
};

// A line of text. Wide enough for a firmware version (31 characters) with a
// word in front of it.
class Label : public Widget {
public:
    void configure(const char* text, Color c = color::kDim,
                   uint8_t scale = 1, Align align = Align::Left);
    void set_text(const char* text);
    void render(Canvas& c) override;
    // Only the characters that changed, when the length did not: the LAP
    // screen's running time is a 288x62 label that changes every frame, and
    // usually in its last two digits.
    int damage(Rect* out, int max) const override;
    void clear_dirty() override { Widget::clear_dirty(); sent_ = text_; }

private:
    TextBuf<48> text_;
    TextBuf<48> sent_;            // what the panel shows
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
    // Only the digits that changed. MINIMAL's speed is 480x210, ~20 ms of
    // wire whole, for what is usually one digit moving.
    int damage(Rect* out, int max) const override;
    void clear_dirty() override {
        Widget::clear_dirty();
        sent_value_ = value_;
        sent_valid_ = valid_;
    }

private:
    TextBuf<8> unit_;
    float      value_ = 0.0f;
    bool       valid_ = false;
    float      sent_value_ = 0.0f;
    bool       sent_valid_ = false;
    int        decimals_ = 0;
    uint8_t    scale_ = 8;
    Color      color_ = color::kText;
};

// A horizontal bar, 0..1. Fuel, throttle.
class Bar : public Widget {
public:
    void configure(Color fill, Color warn_fill, float warn_above = 2.0f);
    void set(float fraction, bool valid = true);
    // Blank, for a screen that shows the bar in only some of its states.
    void set_hidden(bool hidden);
    void render(Canvas& c) override;

private:
    bool  hidden_ = false;
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
    // The deepest lean each way, both in positive degrees. Handed in from
    // the Frame (Lap::max_lean_*), not kept here: a screen is rebuilt every
    // time it is entered, and peaks kept in this widget were lost each time.
    void set_peaks(float left_deg, float right_deg);
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
    static constexpr size_t kRows = 12;
    void configure(uint8_t visible_rows, uint8_t scale = 1);
    void set_row(size_t i, const char* left, const char* right);
    void set_row_count(size_t n);
    void set_focus(int focus);          // -1 for none
    void clear_rows();
    size_t row_count() const { return rows_; }
    void render(Canvas& c) override;
    // Only the rows that changed. A DIAG table is 452x264, ~24 ms of wire
    // whole, and one changing number used to send all of it every frame.
    int damage(Rect* out, int max) const override;
    void clear_dirty() override { Widget::clear_dirty(); row_mask_ = 0; }

private:
    size_t first_visible(int focus) const;
    int16_t line_px() const;
    void mark_row(size_t r);

    TextBuf<20> left_[kRows];
    TextBuf<14> right_[kRows];
    size_t      rows_ = 0;
    int         focus_ = -1;
    uint8_t     visible_ = kRows;
    uint8_t     scale_ = 1;
    uint16_t    row_mask_ = 0;     // rows changed since the last send
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
    // Points ever added, which keeps moving after a full buffer's count()
    // stops: what the map watches to know the trace has grown.
    virtual size_t total() const { return count(); }
};

class MapView : public Widget {
public:
    void set_source(const IPointSource* src);
    // Called every frame. The marker is drawn on the newest trace point,
    // which is where the bike is, with a pointer the way it was going (from
    // the trace itself, so it always agrees with the line under it, and
    // moves when the trace is redrawn rather than on every fix); `valid` is
    // whether to draw it at all.
    void set_live(bool valid);
    void render(Canvas& c) override;

private:
    // How many points to draw and the box that fits them, worked out once
    // per redraw rather than once per strip: a 286x276 map is five strips,
    // each walking 4096 PSRAM points twice, and a point landing between two
    // strips rescaled the second against the first. The strips of one
    // redraw now share one fit.
    void refit();

    const IPointSource* src_ = nullptr;
    static constexpr size_t kRedrawPoints = 10;
public:
    // Between the frame and the trace, for the marker and its pointer.
    static constexpr int16_t kMarkerMargin = 14;
private:
    size_t drawn_total_ = 0;
    bool  valid_ = false;
    bool  fit_stale_ = true;
    size_t fit_n_ = 0;
    float fit_min_x_ = 0, fit_max_x_ = 0, fit_min_y_ = 0, fit_max_y_ = 0;
    bool  fit_ok_ = false;          // at least one readable point
};

// The tach bar: an outline up the left edge of the speed screen that flares
// out along the top, with a horizontal marker riding up it as the revs rise.
//
// The marker only ever moves in y. Where the outline flares it simply gets
// longer — there is no corner to turn, so there is no join to make smooth.
// The previous version ran a fill up one leg and then along another at right
// angles to it, and you could see exactly where it changed its mind.
//
// It is a widget rather than something the screen paints because it moves,
// and only widgets are tracked for redraw — chrome painted by a screen is
// refreshed on a full repaint and never in between, which on a partial
// update display means it would simply stop.
class TachBar : public Widget {
public:
    // The rows this instance is responsible for. Two of them cover the bar,
    // the straight run and the flare, so a marker moving within one does not
    // repaint the other.
    void configure(int16_t y_top, int16_t y_bottom);

    void set(float fraction);      // 0..1 of the rev range

    // The rpm before the last shift (Engine::rpm_peak), as the same 0..1:
    // a white line across the bar where the marker got to. 0 or less for
    // none. Redrawn as its own rows, apart from the marker's.
    void set_peak(float fraction);

    // A 1px mark at every 1000 rpm up to `base_rpm` (the same full-scale
    // value `fraction` in set() is measured against), drawn over the
    // marker rather than under it — a rider glancing at the bar mid-shift
    // needs the thousands, not just the lit fraction, and a mark that
    // vanished under the marker at the exact moment it mattered would be
    // worse than not having it.
    void set_scale(uint16_t base_rpm);

    void render(Canvas& c) override;
    // The rows the marker left and arrived on: ~600 px a move instead of the
    // whole 25x295 stem, which was 1.5 ms of wire nearly every frame.
    int damage(Rect* out, int max) const override;
    void clear_dirty() override {
        Widget::clear_dirty();
        moved_ = false;
        peak_sent_y_ = peak_y_;
    }

    // How wide the bar is at a given row. One function for the track and the
    // marker both, so the two can never disagree about where the edge is,
    // and continuous at the join by construction: at y == kFlareH it is
    // kStemW from either side.
    static int16_t width_at(int16_t y);

    static constexpr int16_t kStemW   = 25;    // the straight run, all the way down
    static constexpr int16_t kFlareH  = 25;    // how far down the flare reaches
    // Width at the very top. Bounded by the shift lamp strip at x=136: the
    // flare cannot grow past it without the two overlapping.
    static constexpr int16_t kFlareW  = 135;
    static constexpr int16_t kMarkerH = 12;
    static constexpr int16_t kTickH   = 1;     // thickness of each 1000-rpm mark
    static constexpr int16_t kPeakH   = 3;     // the held peak's line

private:
    // A pixel of movement is not worth a repaint of the whole strip.
    static constexpr int16_t kStepPx = 2;

    bool covers(int16_t marker_y) const;

    // 12000 rpm (the top of shift_base_rpm's configurable range) is 12
    // marks; a couple of spares cost nothing.
    static constexpr size_t kMaxTicks = 16;

    int16_t y_top_    = 0;
    int16_t y_bottom_ = 0;
    uint16_t base_rpm_ = 0;
    int16_t  tick_y_[kMaxTicks] = {};
    uint8_t  tick_count_ = 0;
    // Off the bar entirely, so the first set() always counts as a move.
    int16_t marker_y_ = -1000;
    // Rows the marker has swept since the last send.
    bool    moved_ = false;
    int16_t moved_top_ = 0;
    int16_t moved_bottom_ = 0;
    // The held peak's top row, and the one on the panel; kNoPeak for none.
    static constexpr int16_t kNoPeak = -1000;
    int16_t peak_y_ = kNoPeak;
    int16_t peak_sent_y_ = kNoPeak;
    // The rows of a peak line at `y` inside this instance's strip, or empty.
    Rect    peak_rows(int16_t y) const;
};

// RACE's tach: a bar across the whole width, filled from the left, lit the
// colour the shift lamps are saying -- the dash colour below the first lamp,
// amber once a lamp is on, red at the flash -- so the bar and the handlebar
// never disagree. Marks every 1000 rpm, drawn over the fill.
//
// A widget, not chrome, because it moves; and only the columns between the
// old edge and the new one go out on a partial update, as TachBar does in y.
class RevBar : public Widget {
public:
    // `full_rpm` is the right-hand end. 0 draws an empty bar.
    void set_scale(uint16_t full_rpm);
    // `lamps` and `flash` are what `ctrl` sent the lamps (Engine::shift_mask,
    // shift_flash).
    void set(float rpm, bool lamps, bool flash, bool valid = true);
    // The rpm before the last shift (Engine::rpm_peak): a white line down
    // the bar where the fill got to. 0 for none.
    void set_peak(float rpm);
    // The rpm as a number, over the left end of the bar in a box of its own,
    // to the nearest 100. Off unless asked for.
    void show_number(bool on);
    void render(Canvas& c) override;
    int damage(Rect* out, int max) const override;

    static constexpr int16_t kStepPx = 2;
    // The number's box: inset from the bar's left end, as tall as the bar
    // allows, wide enough for "12000" at kNumberScale.
    static constexpr uint8_t kNumberScale = 4;

private:
    enum class Shade : uint8_t { Normal, Lamps, Flash };
    int16_t edge_for(float rpm) const;

    uint16_t full_rpm_ = 0;
    int16_t  edge_ = -1;              // filled columns, from bounds.x
    Shade    shade_ = Shade::Normal;
    bool     valid_ = false;
    bool     moved_ = false;
    int16_t  moved_l_ = 0;
    int16_t  moved_r_ = 0;
    // The held peak's column, and the one on the panel; -1 for none.
    int16_t  peak_x_ = -1;
    int16_t  peak_sent_x_ = -1;
    Rect     peak_cols(int16_t x) const;
    // The number: shown at the last set(), and on the panel; -1 for none.
    bool     number_ = false;
    int32_t  number_rpm_ = -1;
    int32_t  number_sent_ = -1;
    Rect     number_box() const;
public:
    void clear_dirty() override {
        Widget::clear_dirty();
        moved_ = false;
        peak_sent_x_ = peak_x_;
        number_sent_ = number_rpm_;
    }
};

// The lean gauge on the speed screen: a half annulus with a marker on it and
// the angle in the middle -- and, around a jump, the jump instead: the
// airtime counting up while in the air, then the airtime and the landing g
// for a few seconds after touching down.
//
// One widget with two faces rather than two widgets swapped in and out of a
// screen's list, because the rectangle is the unit of redraw: two widgets
// sharing a rectangle would each leave the other's pixels behind on a
// partial update, which is exactly the invariant test_ui exists to hold.
class LeanGauge : public Widget {
public:
    // Lean; a jump (in the air, then landed); a wheelie or stoppie (under
    // way, then scored), which SPEED shows here instead of as a toast.
    enum class Face : uint8_t { Lean, Air, Landed, Trick, TrickDone };

    void set(float lean_deg, bool valid);

    // Which face, and the jump's numbers for the other two. How long the
    // landed face stays up is the screen's business -- a widget has no
    // clock. In the air the airtime is shown to a tenth, so the tile is
    // redrawn ten times a second rather than on every frame.
    void set_jump(Face face, uint32_t airtime_ms, float landing_g);
    // The trick faces, as text the screen has already formatted: a label
    // (WHEELIE, or the grade), the big line (the time, or the points), and
    // a detail line under it. Redrawn when the face, the label or the big
    // line changes; under way the big line is the time to a tenth, so the
    // detail (angle, points) rides along at ten frames a second rather than
    // costing the whole 248x126 gauge on every frame.
    void set_trick(Face face, const char* label, Color label_color, const char* big,
                   const char* detail);
    Face face() const { return face_; }

    void render(Canvas& c) override;
    // On the lean face, only where the marker was, where it is, and the
    // number: the whole 248x126 gauge was 6.2 ms of wire nearly every
    // frame of a ride.
    int damage(Rect* out, int max) const override;
    void clear_dirty() override {
        Widget::clear_dirty();
        sent_lean_ = lean_deg_;
    }

private:
    void render_jump(Canvas& c);
    void render_trick(Canvas& c);

    // The geometry render() draws with, so damage() names the same pixels.
    struct Geom { int16_t cx, cy, r, ri, ov; uint8_t scale; };
    Geom geom() const;
    Rect marker_box(const Geom& g, float lean_deg) const;
    Rect number_box(const Geom& g) const;

    float    lean_deg_ = 0.0f;
    float    sent_lean_ = 0.0f;
    bool     valid_ = false;
    Face     face_ = Face::Lean;
    uint32_t airtime_ms_ = 0;
    float    landing_g_ = 0.0f;
    TextBuf<16> trick_label_;
    TextBuf<12> trick_big_;
    TextBuf<24> trick_detail_;
    Color       trick_color_ = color::kAccent;
};

// One line of what is playing: an icon and a name that scrolls if it is too
// long for the space.
//
// A row rather than a block of three, because a widget is the unit of
// redraw and only the rows that overflow need repainting — a scrolling album
// title should not be dragging a static artist name down the SPI bus with
// it. The icon lives in the row rather than in the screen's background,
// because chrome is only redrawn on a full repaint: an icon painted there
// would still be sitting next to nothing after the music stopped.
class MediaRow : public Widget {
public:
    enum class Kind : uint8_t { Song = 0, Album, Artist, Lyric };

    // `scale` defaults to the size the speed screen has always used; a
    // dedicated media screen with room to spare passes a bigger one. The
    // icon stays its native size either way — it is a small fixed glyph,
    // not something meant to scale with the text next to it.
    //
    // `wrap` trades the scroll for a second line: text wider than the row
    // breaks at the last space that fits and carries on below, and never
    // moves. For the lyric, which changes every few seconds and has to be
    // read whole at a glance; a belt would still be carrying the last line
    // when the next arrived. Give the row two lines' height
    // (kWrapLineH * scale * 2). Anything past two lines is cut.
    void configure(Kind k, Color text_color, uint8_t scale = kDefaultScale,
                   bool wrap = false);

    // `valid` is false when nothing has ever arrived, or when the phone has
    // gone away — a dash still showing what was playing forty minutes ago is
    // worse than one showing nothing.
    //
    // `now_ms` drives the scroll. A widget has no clock, and the Frame has
    // the time in it already, so the screen passes it down.
    void set(const char* text, bool valid, uint32_t now_ms);

    void render(Canvas& c) override;

    static constexpr int16_t kRowH  = 20;   // suggested height at kDefaultScale
    static constexpr uint8_t kDefaultScale = 2;
    static constexpr int16_t kTextX = 21;   // clear of the widest icon
    static constexpr int16_t kWrapLineH = 8;   // per unit of scale: 7 + 1 gap

private:
    // Where the text restarts behind itself, so the loop reads as a belt
    // rather than as a jump.
    static constexpr int16_t  kGapPx      = 36;
    static constexpr uint32_t kSpeedPxPerS = 24;   // two characters a second
    // Movement is quantised to this. At 2 px the scroll still looks smooth
    // and the row repaints twelve times a second instead of thirty.
    static constexpr int16_t  kStepPx     = 2;

    TextBuf<60> text_;
    Kind        kind_    = Kind::Song;
    Color       color_   = color::kText;
    uint8_t     scale_   = kDefaultScale;
    bool        wrap_    = false;
    bool        valid_   = false;
    int16_t     offset_  = 0;      // 0 whenever the text fits
};

// The media block's progress ring: how far into the track as an arc from
// twelve o'clock, the elapsed and total time inside it, and a play/pause
// glyph. Sits in the one patch of the speed screen §11.1 media left empty —
// left of the song/album/artist rows, under the lean gauge.
//
// A widget has no clock, so it does not extrapolate `elapsed_ms` itself —
// the screen does that from `Frame.t_ms` and `Media.position_at_ms` every
// tick, the same way the lean gauge's jump hold works from `land_ms`. This only
// draws whatever `elapsed_ms` it was last handed, quantised to the second
// so a 20-50 Hz update loop still only repaints the ring once a second.
class MediaProgress : public Widget {
public:
    // `state` is the raw §11.1 tag: 0 stop, 1 play, 2 pause. `valid` is
    // false when there is no media at all — same gate `MediaRow` uses.
    void set(uint32_t elapsed_ms, uint32_t duration_ms, uint8_t state,
             bool valid);
    void render(Canvas& c) override;

private:
    uint32_t elapsed_ms_  = 0;
    uint32_t duration_ms_ = 0;
    uint8_t  state_       = 0;
    bool     valid_       = false;
};

// The current lyric line's picture, matched by album title against a
// handful of albums baked into flash (`ui/album_art.hpp`) — see that
// header for why this never touches BLE. Blank when the album has no
// baked-in cover, which is nearly always true: this is a rider's ~10
// favourite albums, not their whole library.
class AlbumArt : public Widget {
public:
    void set(const char* album, bool valid);
    void render(Canvas& c) override;

private:
    TextBuf<60>              album_;
    bool                      valid_ = false;
    const album_art::Cover*  cover_ = nullptr;
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

// "DEV", in a corner, on every frame a dev image is running, so nobody rides
// a test build thinking it is the real one.
// The NAV screen's pointer: an arrow in a ring, pointing at the waypoint.
// `rel_deg` is from the top of the ring, clockwise. Heading-up when riding,
// so straight up is "ahead"; north-up when parked, with an N on the ring.
// `--` with no fix or no waypoint.
class NavArrow : public Widget {
public:
    void set(float rel_deg, bool heading_up, bool valid);
    void render(Canvas& c) override;
    // Where the chevron was and where it is, not the whole 236x236 dial:
    // heading-up while riding, it turns on nearly every fix.
    int damage(Rect* out, int max) const override;
    void clear_dirty() override { Widget::clear_dirty(); sent_deg_ = rel_deg_; }

private:
    Rect chevron_box(int16_t deg) const;

    int16_t rel_deg_ = 0;           // whole degrees: finer only redraws noise
    int16_t sent_deg_ = 0;
    bool    heading_up_ = false;
    bool    valid_ = false;
};

// Wheelspin and lockup (core/slip), as a bar across the top of the screen:
// amber for spin, red for a locked wheel, filling with how much, and named
// in the middle. Drawn over whatever the screen has there, only while
// there is something to show.
class SlipBar : public Widget {
public:
    void set(core::slip::Kind kind, uint8_t pct);
    bool active() const { return kind_ != core::slip::Kind::None; }
    core::slip::Kind kind() const { return kind_; }
    uint8_t pct() const { return pct_; }
    void render(Canvas& c) override;

private:
    core::slip::Kind kind_ = core::slip::Kind::None;
    uint8_t          pct_  = 0;
};

// STAT -> HEART's last ten minutes: the line in the colour of each stretch's
// zone, broken where there was no pulse, faint lines at the zone edges, and
// a scale fitted to the range ridden. Reads a core::hr::History it does not
// own (ui::App keeps it, so it outlives the screen) and redraws when a
// sample lands, every 2 s, or the max HR changes.
class HrGraph : public Widget {
public:
    void set_source(const core::hr::History* h) { if (h != src_) { src_ = h; touch(); } }
    void set(uint16_t zone_max_bpm);
    void render(Canvas& c) override;

private:
    const core::hr::History* src_ = nullptr;
    uint32_t seq_ = 0;
    uint16_t max_ = 0;
};

class DevTag : public Widget {
public:
    void render(Canvas& c) override;
};

// A strip along the bottom of every screen, for a few seconds: a trick that
// landed, a timed run that finished, a call or a message. Under the alert,
// which outranks it, and gone by itself: unlike the alert it asks for no
// press, because a rider who has to dismiss a high score will stop wanting
// them. A big line and a small one, in the colour of what it is.
class Toast : public Widget {
public:
    // `hint`, if any, is a few words at the top-right corner, small: how to
    // clear one that waits for a press.
    void show(const char* big, const char* small, Color c, const char* hint = "");
    void hide();
    bool active() const { return active_; }
    void render(Canvas& c) override;

private:
    TextBuf<28> big_;
    TextBuf<44> small_;
    TextBuf<12> hint_;
    Color       color_ = color::kAccent;
    bool        active_ = false;
};

}  // namespace ui
