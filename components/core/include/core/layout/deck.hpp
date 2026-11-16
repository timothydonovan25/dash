// The rider's arrangement of modes and screens, and the screens they drew
// themselves (docs/custom-screens.md §2).
//
// A Deck is plain data: the stock one is compiled in (default_deck()), a
// custom one is a `.dui` file on the card in the format encode() writes.
// Everything that reaches the dash from a browser or a card goes through
// decode(), which validates it; nothing else makes a Deck from bytes.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::layout {

// --- limits ------------------------------------------------------------------
inline constexpr uint8_t  kMaxModes          = 8;    // riding modes; DIAG and CONFIG are extra
inline constexpr uint8_t  kMaxScreensPerMode = 12;
inline constexpr uint8_t  kMaxCustom         = 16;
inline constexpr uint16_t kMaxChrome         = 512;
inline constexpr uint16_t kMaxReadouts       = 192;
inline constexpr uint8_t  kMaxReadoutsPerScreen = 16;
inline constexpr uint8_t  kMaxSparksPerScreen   = 2;
inline constexpr uint8_t  kMaxChromePerScreen   = 255;
inline constexpr uint8_t  kMaxImages         = 8;
inline constexpr uint8_t  kMaxImageSide      = 64;
inline constexpr uint16_t kImagePool         = 4096;
inline constexpr size_t   kMaxEncoded        = 48 * 1024;

inline constexpr int16_t  kGlassW = 480;
inline constexpr int16_t  kGlassH = 320;
inline constexpr int16_t  kTitleH = 30;     // ui::chrome::kBodyTop

inline constexpr uint8_t  kNameLen    = 8;  // a mode's or a screen's, NUL included
inline constexpr uint8_t  kTextLen    = 24; // a Text element's
inline constexpr uint8_t  kCaptionLen = 16; // a readout's own caption

// --- screens a mode can hold ---------------------------------------------------
// The stable numbers of the built-in screens (ui::ScreenId has the same
// values, checked by static_assert in ui/app.cpp). Appended, never reordered.
enum Builtin : uint8_t {
    kNone = 0,
    kSpeed, kMedia, kTrip, kTripMap, kNav, kMinimal, kPhone,
    kRace, kLap, kLapHistory, kTrackMap, kSession,
    kAllData, kEngine, kAttitude, kPerf, kStunt, kHeart,
    // DIAG's and CONFIG's: not allowed in a riding mode.
    kDiagMenu, kSensors, kGps, kFusion, kSystem, kPower, kBus, kRadio, kUpdate,
    kConfig, kGearCal,
    kBuiltinEnd,
};
inline constexpr uint8_t kFirstRiding = kSpeed;
inline constexpr uint8_t kLastRiding  = kHeart;
// A custom screen in a mode: kCustomBase + its index in Deck::screens.
inline constexpr uint8_t kCustomBase = 0x80;

const char* builtin_name(uint8_t id);   // "SPEED"; null for none

// --- what a screen is drawn from ---------------------------------------------------
using Color = uint16_t;   // RGB565, as ui::Color

enum class Shape : uint8_t {
    Line = 0, Rect, FillRect, RoundRect, FillRoundRect,
    Circle, Disc, Arc, Triangle, Text, Icon,
    kCount
};

// Static: drawn once per full repaint, under the readouts.
//  - Line from (x, y) to (x + w, y + h).
//  - Rect, round rect: the box x, y, w, h; `radius` for the round ones.
//  - Circle, disc: inscribed in the box.
//  - Arc: a ring inscribed in the box, `radius` thick, a0..a1 degrees
//    clockwise from north.
//  - Triangle: (x, y), (x + w, y + h), (x3, y3).
//  - Text: at (x, y), `scale`, `align` (0 left, 1 centre, 2 right of x).
//  - Icon: `icon` < kUserImage is a ui::icon; >= is Deck::images[icon - kUserImage].
struct Chrome {
    Shape   shape = Shape::Rect;
    uint8_t radius = 0;
    uint8_t scale = 1;
    uint8_t align = 0;
    Color   color = 0;
    int16_t x = 0, y = 0, w = 0, h = 0;
    int16_t x3 = 0, y3 = 0;
    int16_t a0 = 0, a1 = 0;
    uint8_t icon = 0;
    char    text[kTextLen] = {};
};
inline constexpr uint8_t kIconCount = 4;      // ui::icon kSong, kAlbum, kArtist, kLyric
inline constexpr uint8_t kUserImage = 0x80;

enum class Style : uint8_t {
    Value = 0,  // caption, then the value and its unit
    Big,        // the value alone, centred, large
    Text,       // the value alone, one line, aligned
    Bar,        // horizontal, lo..hi
    Gauge,      // a 240-degree arc, lo..hi, the value in the middle
    Lamp,       // a disc, lit while the value is non-zero (or past warn)
    Spark,      // a trace of the last w samples, lo..hi
    kCount
};

// Live: one widget each, repainted when what it shows changes.
struct Readout {
    enum Flags : uint8_t {
        kCaption   = 1 << 0,   // Value, Lamp: draw the caption
        kUnit      = 1 << 1,   // draw the unit
        kWarn      = 1 << 2,   // ink turns warning past `warn`
        kAlarm     = 1 << 3,   // ink turns alarm past `alarm`
        kBelow     = 1 << 4,   // "past" means below (a battery, a tyre), not above
    };
    uint16_t field = 0;        // core::layout::FieldDef::id
    Style    style = Style::Value;
    uint8_t  scale = 2;
    int8_t   decimals = -1;    // -1: the field's own
    uint8_t  flags = kCaption | kUnit;
    uint8_t  align = 0;
    int16_t  x = 0, y = 0, w = 0, h = 0;
    Color    ink = 0;
    Color    ground = 0;       // what it paints its box with
    // In the field's native unit (mph, C, psi), whatever CONFIG -> Units says.
    float    lo = 0.0f, hi = 100.0f;
    float    warn = 0.0f, alarm = 0.0f;
    char     caption[kCaptionLen] = {};   // empty: the field's own
};

struct Screen {
    enum Flags : uint8_t { kTitle = 1 << 0 };
    char     name[kNameLen] = {};
    uint8_t  flags = kTitle;
    Color    background = 0;
    uint16_t e_action = 0;      // CONFIG key of an Action item short E runs; 0 none
    uint16_t chrome_first = 0;  // into Deck::chrome
    uint8_t  chrome_n = 0;
    uint16_t readout_first = 0; // into Deck::readouts
    uint8_t  readout_n = 0;
};

struct Image {
    uint8_t  w = 0, h = 0;      // 1 bit a pixel, rows padded to bytes, MSB first
    uint16_t offset = 0;        // into Deck::image_bits
};

struct ModeDef {
    char    name[kNameLen] = {};
    uint8_t count = 0;
    uint8_t screens[kMaxScreensPerMode] = {};   // Builtin, or kCustomBase + index
};

struct Deck {
    uint8_t  mode_count = 0;
    ModeDef  modes[kMaxModes];
    uint8_t  screen_count = 0;
    Screen   screens[kMaxCustom];
    uint16_t chrome_count = 0;
    Chrome   chrome[kMaxChrome];
    uint16_t readout_count = 0;
    Readout  readouts[kMaxReadouts];
    uint8_t  image_count = 0;
    Image    images[kMaxImages];
    uint16_t image_bytes = 0;
    uint8_t  image_bits[kImagePool] = {};
};

// Today's modes and screens, exactly: what the dash shows with no card.
void default_deck(Deck& out);

// Why a deck was refused, for the page and the toast. Static strings.
struct Error {
    const char* what = "";
    int         index = -1;   // the mode, screen, element or readout, -1 none
};

// Whether `id` names a field this firmware has, and E's action key an
// Action item. Supplied by the caller so the checks stay testable; the
// defaults use the generated field table and core::config.
struct Known {
    bool (*field)(uint16_t id) = nullptr;
    bool (*action)(uint16_t key) = nullptr;
};

bool validate(const Deck& d, Error* why = nullptr, const Known& k = Known{});

// The `.dui` format: "DUI1", counts, the parts in order, CRC-32 last.
// Returns the length written, 0 if `n` is too small.
size_t encode(const Deck& d, uint8_t* out, size_t n);
// Decodes AND validates. On false, `out` is unspecified.
bool   decode(const uint8_t* in, size_t n, Deck& out, Error* why = nullptr,
              const Known& k = Known{});

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0);

// What the editor and validate() take a Text element's box to be: the 5x7
// font's 6-pixel advance and 8-pixel line at `scale`.
void chrome_box(const Chrome& c, int16_t& x0, int16_t& y0, int16_t& x1, int16_t& y1);

}  // namespace core::layout
