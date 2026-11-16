# Custom screens and modes — plan

Status: **built in 2.23** (2026-11-14): host-tested and run in the
simulator, **not yet run on the board**. What exists and how to use it is
`features.md` §36; this file keeps the plan and its reasons. Where the build
differs from the plan, §12 says so.

The goal: from the hotspot page at `http://192.168.4.1` (features §25) a
rider can

1. reorder, hide and move screens between modes;
2. make, rename, reorder and delete modes;
3. **draw new screens** in an editor that looks and works like
   [Lopaka](https://lopaka.app): a pixel canvas, a tool palette, layers and
   a properties panel. A screen is made of
   - **chrome**: lines, rectangles, rounded rectangles, circles, arcs,
     triangles, static text, icons and small 1-bit images, drawn once per
     full repaint, the way built-in screens draw theirs; and
   - **readouts**: widgets bound to a datapoint, redrawn when it changes;
4. bind a readout to **any datapoint in the Frame**, all ~270 fields plus
   strings and per-wheel and per-device values, not a hand-picked list;
5. **edit live**: while the editor is open, the dash shows the screen being
   drawn, change by change, with the bike's real values in the readouts.

**Where layouts live.** The stock layout is compiled into the `.bin`
(`core::layout::default_deck()`). Custom layouts are files on the SD card.
No card, or no layout chosen, or a file that does not check out: the dash
runs the stock layout. Nothing about layouts is kept in NVS.

---

## 1. What exists today

| Piece | Where | Today |
|---|---|---|
| Modes | `core::nav::Mode` | Fixed enum: Cruise, Track, Stat, Diag, Config |
| Screens per mode | `core::nav::Layout::screens[] = {7, 5, 6, 9, 1}` | Fixed counts |
| (mode, index) → screen | `ui::App::screen_for()` | A `switch` per mode |
| Building a screen | `ui::App::build(ScreenId)` | A `switch`, into one slot sized by `ui::AllScreens` |
| Drawing | `ui::Canvas` | `fill_rect`, `draw_rect`, `draw/fill_round_rect`, `draw_line`, `draw/fill_circle`, `fill_arc`, `fill_triangle`, `draw_text` (5×7 font × integer scale, aligned), `draw_bitmap` (1-bit), `draw_image` (RGB565) |
| Colours | `ui::theme` | Named roles (`background`, `panel`, `rule`, `primary`, `secondary`, `warning`, `alarm`, `selection` ...), remapped by CONFIG → Palette |
| Icons | `ui/icons.hpp` | `Bitmap` constants (`kSong`, `kAlbum` ...) |
| The data | `core::state::Frame` (`state/slots.hpp`, `state/vehicle.hpp`) | 25 sub-structs, ~270 scalar fields, a dozen strings, small arrays (TPMS wheels, health per `Dev`) |
| Web page | `services/src/wifi_page.cpp` (`kPage`, ~33 kB), handlers in `wifi_service.cpp` | 15 URI handlers, `max_uri_handlers = 16` |
| The card | `hal::ICard`, owned by `storage` (core 1) | `write_file`, `rename`, `list`, `read_at`, `remove`; the map upload already writes a `.tmp` and renames it (`route_file_put`) |

How a widget repaints matters for the editor (`App::draw_damage`): a changed
widget calls only its own `render()`, which paints its whole bounds opaque.
**Chrome under a readout is wiped the first time the value changes.** That
gives §6's overlap rule.

---

## 2. Data model: the *deck*

`core::layout::Deck` holds everything a rider can change. It lives in
`components/core/` (no ESP headers, host-tested), is trivially copyable,
and goes in PSRAM: `ui::App` sits on render's stack and gets a pointer, the
way it already gets `hr_history_`.

```cpp
namespace core::layout {

struct Entry { uint8_t id; };          // ui::ScreenId (now stable numbers) or 0x80 + custom index

struct ModeDef {
    char    name[8];                   // shown in the title bar
    uint8_t count;
    Entry   screens[kMaxScreensPerMode];
};

// A colour is a theme role (so CONFIG -> Palette still applies) or, with
// the top bit of `kind` set, a literal RGB565.
struct Paint { uint8_t kind; uint16_t rgb565; };

enum class Shape : uint8_t {
    Line, Rect, FillRect, RoundRect, FillRoundRect,
    Circle, Disc, Arc, Triangle, Text, Icon, Image,
};

struct Chrome {                        // static: drawn in draw_background()
    Shape   shape;
    Paint   paint;
    int16_t x, y, w, h;                // Line: x,y -> x+w,y+h. Triangle: plus p3
    int16_t x3, y3;
    uint8_t radius;                    // round rects; arc thickness
    int16_t a0, a1;                    // arc start/end degrees
    uint8_t font, scale, align;        // Text
    uint16_t text;                     // offset into the deck's string pool
    uint8_t icon;                      // ui icon index, or user image index
};

enum class Style : uint8_t {
    Value,     // ValueBox: caption, number, unit
    Big,       // BigNumber
    Text,      // a string field, or an enum's name
    Bar,       // horizontal, min..max
    Gauge,     // arc, min..max (fill_arc)
    Lamp,      // bool or threshold: a dot or a filled shape, on/off colour
    Duration,  // _ms fields: m:ss.t
    Spark,     // sparkline; at most 2 per screen (each has a trace buffer)
};

struct Readout {                       // live: a widget
    uint16_t field;                    // core::layout::Field id, §3
    Style    style;
    int16_t  x, y, w, h;
    Paint    ink, ground;              // ground = what it paints its box with
    uint8_t  font, scale, decimals;
    float    lo, hi;                   // Bar / Gauge range
    float    warn, alarm;              // ink turns warning / alarm past these
    uint16_t caption;                  // string pool; empty: the field's own
};

struct Screen {
    char     name[8];
    bool     title;                    // draw the title bar
    uint16_t e_action;                 // CONFIG key of an Action item short E runs; 0: none
    Paint    background;
    uint8_t  chrome_n, readout_n;
    uint16_t first_chrome, first_readout;   // into the deck's pools
};

struct Deck {
    uint8_t  mode_count;  ModeDef modes[kMaxModes];
    uint8_t  screen_count; Screen screens[kMaxCustom];
    // Pools, shared by every screen, so one busy screen and many plain ones
    // fit the same budget.
    Chrome   chrome[kMaxChrome];       // 512
    Readout  readouts[kMaxReadouts];   // 192
    char     strings[kStringPool];     // 2 kB
    uint8_t  images[kImagePool];       // 2 kB of 1-bit bitmaps (the Paint tool)
};

Deck default_deck();                   // today's layout, exactly
bool validate(const Deck&, Error* why);
size_t encode(const Deck&, uint8_t* out, size_t n);    // packed, versioned, CRC
bool   decode(const uint8_t* in, size_t n, Deck& out, Error* why);
}
```

The in-RAM `Deck` is about 30 kB in PSRAM. The **encoded** form (the file
on the card) stores only the elements in use, so a plain layout is a few
kB; the pools above are the real limit, and the editor shows how full each
is.

### Pinned modes

DIAG and CONFIG are not in the deck. They are always the last two modes and
cannot be hidden, renamed or edited: CONFIG has the Wi-Fi switch, so a deck
without it would lock the rider out of the page that fixes it, and DIAG has
UPDATE and its own menu. **Mode 0 is home**: long A, the three-quick-key-ons
escape and a blank `kKeyUiPlace` all land on mode 0, screen 0.

---

## 3. Every datapoint: the generated field table

A readout names a **field**. The table of fields is **generated from the
Frame's headers**, so nothing in Frame can be left out and nobody keeps a
list up to date by hand.

### The generator

`tools/frame_fields.py` reads `core/state/slots.hpp`, `vehicle.hpp` and the
headers they pull in (`shift::LedFaults`, `crash::State`), walks `Frame`
member by member, and writes `components/core/src/layout_fields.gen.cpp`:

```cpp
// generated by tools/frame_fields.py -- do not edit
{ 1, "engine.rpm",       Type::F32, Q::Rpm,   0, [](const Frame& f){ return V(f.engine.rpm); } },
{ 2, "engine.neutral",   Type::Bool, Q::None, 0, [](const Frame& f){ return V(f.engine.neutral); } },
{ 3, "engine.engine_c",  Type::F32, Q::Temp,  0, [](const Frame& f){ return V(f.engine.engine_c); } },
...
{ 140, "tpms.wheel[0].psi", ... },
{ 141, "health.dev[Imu].state", Type::Enum, ... },
{ 200, "media.title",    Type::Str, ... [](const Frame& f){ return V(f.media.title); } },
```

- **Getters, not `offsetof`.** One small lambda per field compiles whatever
  the struct looks like. A `V` holds a float, a bool, an enum value or a
  `const char*`.
- **Nested structs** flatten to dotted paths. **Arrays** of up to 8 expand
  per element, by enum name where an enum indexes them (`health.dev[Gnss]`).
  Bigger arrays are series (lap lists, pace strings, map traces), not
  datapoints; built-in screens keep those (§11).
- **Units from the name.** The repo already puts units in names (CLAUDE.md),
  so `_mph`/`_kph` → speed, `_mi`/`_ft` → distance, `_c` → temperature,
  `_kpa`, `_psi` → pressure, `_deg`, `_g`, `_pct`, `_ms` → duration. Each
  goes through `core::units` so readouts follow CONFIG → Units. Fields
  whose names carry no unit (`vbat`, `rpm`) are fixed in
  `tools/frame_fields.override`, which also holds captions ("Lean" for
  `att.lean_deg`), enum names and the blackbox column name where there is
  one (`MPH`, `FrontPsi`), so RaceRender users find what they know.
- **Validity.** A field marked with a health device in the override file
  (`gnss.*` → `Dev::Gnss`) draws `--` when that device is not working, like
  `AllDataScreen`. A missing reading is never shown as the last value held.

### Stable ids

The deck stores ids, so an id must mean the same field forever. The
generator keeps a registry, `components/core/layout_fields.ids`
(`id path`), committed:

- a new field gets the next free id, appended;
- a renamed field is a new id; the old line is kept, marked `retired`, so
  an old deck's readout draws `?` rather than the wrong number;
- ids are never reused or renumbered (the same rule as CONFIG keys).

### The checks

`ctest` runs `frame_fields.py --check`: it fails if the generated file is
out of date with the headers (a field added to Frame and not regenerated)
or if the registry was renumbered. So "every datapoint" stays true without
anyone remembering it. `test_layout` also checks that every override names
a field that exists.

The table is served to the page as `GET /api/fields` (id, path, caption,
type, unit, enum names), streamed in chunks; about 20 kB of JSON.

---

## 4. The custom screen on the dash

`ui::CustomScreen`, one more entry in `ScreenId`, `ui::AllScreens` and
`App::build()`, handed a `const Deck*` and its screen index.

- **`draw_background()`** paints the screen's background, then each chrome
  element in layer order with the `Canvas` primitives above. Chrome only
  repaints on a full repaint, which is what chrome is for.
- **Readouts are widgets.** Each readout is built in place from a fixed
  array of slots, each sized for the largest allowed widget (no heap after
  boot). `Value`, `Big`, `Bar` reuse `ValueBox`, `BigNumber`, `Bar`; `Text`
  uses `Label`; `Lamp` uses `StatusDot` or a small new widget; `Gauge` is a
  new arc widget on `fill_arc`; `Spark` uses `Sparkline`. Existing widgets
  gain a `ground` colour so a readout can sit inside a panel of another
  colour (today they paint `theme::background`).
- **`update(Frame)`** calls each readout's field getter and sets its widget,
  applying units and the warn/alarm colours. A pure function of one Frame
  (rule 4).
- **Short E runs one action** (`e_action`): the key of an `Action` item in
  the CONFIG table (Reset trip A, Clear laps, Set line here, Test lamps
  ...). `handle()` posts that item's `ConfigItem::action` through
  `ctx.commands`, exactly as `ConfigScreen` does; an item with `confirm`
  wants E twice, any other button cancels, and a small hint along the
  bottom says `E again`. Storing the CONFIG **key**, not the command id,
  means the binding means the same thing in every firmware (keys are
  permanent). Long E stays nav's (the overlay menu); C and D do nothing on a
  custom screen.
- **Limits**: 16 readouts on a screen (render's `kMaxDamage` is 64 for the
  whole frame), 2 sparklines. `sizeof(CustomScreen)` must stay at or under
  the largest built-in screen, or render's stack grows for every screen:
  checked by the `static_assert` in `make<T>()`.

---

## 5. Navigation changes

- `nav::Layout` becomes `{ mode_count; screens[kMaxModes + 2]; }`, filled
  from the deck plus DIAG (9) and CONFIG (1).
- `State::mode` becomes an index. The many `mode == Mode::Config` checks go
  through `is_config(layout, idx)` / `is_diag(layout, idx)`.
- `step()` is otherwise unchanged: A is `(mode + 1) % mode_count`, long A is
  0, B walks `screens[mode]`.
- `App::screen_for()` becomes a lookup in the deck; `mode_name()` reads its
  names.
- `resume_place()` already drops an out-of-range place.
- `ScreenId` values become stable numbers (the deck stores them); new ones
  are appended, never inserted.

`test_nav`, `test_ui` and the sim must show no difference with
`default_deck()`.

---

## 6. Validation (the trust boundary)

A deck comes from a browser or from a file on a card that may have been
hand-edited, half-written or made by another firmware. `decode()` +
`validate()` refuse, with a reason (shown on the page, or as a toast on the
dash for a card file):

- bad magic, version, CRC or length; bigger than the pools;
- no riding mode, a mode with no screens, a name not printable ASCII;
- a screen id that is neither a known built-in nor a custom index in range
  (the same built-in or custom screen may appear in several modes);
  DIAG pages, CONFIG or GearCal named in a riding mode;
- counts over their maxima; a pool offset or string past its end; an icon
  or image index out of range; a font or scale the firmware lacks;
- a readout off the glass, zero-sized, or **overlapping another readout**;
- **chrome that crosses a readout's box**, unless the chrome wholly contains
  the box (a panel behind the value). The editor sets the readout's `ground`
  to that panel's colour when it is dropped in, so this is usually invisible
  to the rider. It is still an error, not a warning, because it would
  break on the first update;
- an `e_action` that is not the key of an `Action` item: refused if it is
  not a CONFIG key at all; a key this firmware has retired leaves E doing
  nothing;
- a field id unknown to this firmware: allowed (an old deck on a newer dash,
  or the reverse) and drawn as `?`.

Chrome may go off the glass; `Canvas` clips. Every rule has a case in
`test/test_layout.cpp`. The page runs the same checks in JavaScript for quick
feedback, but the dash's checks are the ones that count.

---

## 7. Storage: the card, and the default in the image

### On the card

```
ui/
  active          one line: the file name in use ("enduro.dui"); missing or empty = stock
  enduro.dui      encoded decks (§2): magic, version, CRC
  track-day.dui
```

- **Many layouts, one active.** The page lists, loads, saves, renames,
  deletes and picks the active one. Downloading a `.dui` is the backup;
  uploading one is the import. A card moved to another bike carries its
  layouts with it.
- **Writes are safe against a pulled card**: the page's upload goes to
  `ui/<name>.tmp`, is decoded and validated, then renamed over the old file
  (the `route_file_put` pattern already used for the map). `active` is
  written the same way. A torn write leaves the old file, never half a new
  one.
- **No flash is written**, so none of the `core::flash::still` rules apply
  to saving a layout. The hotspot is only up while parked anyway.
- Names: `[a-z0-9-]{1,16}.dui`, checked like `valid_game_name`.

### When the dash uses which

| Situation | Layout shown |
|---|---|
| No card at boot, card on the laptop (USB), card failed to mount | stock |
| Card, no `ui/active` or it is empty | stock |
| Card, `ui/active` names a file that is missing or fails `validate()` | stock, plus a toast naming the reason |
| Card, good file | that file |
| Card pulled while running (or Eject) | **the layout already loaded stays** until key-off; the next boot without a card is stock |
| Card put back | its active file is read again (a changed one is picked up) |
| The editor's live draft (§8) | the draft, until it is saved, abandoned or times out |

At boot the card mounts after `render` starts. To avoid starting on stock
and jumping a second later, render holds the 1 s splash (`kSplashMs`) until
`storage` has answered (deck or "none") or the splash is over, whichever is
first. A card slower than that starts on stock and switches when it is
read. `kKeyUiPlace` is applied to whichever deck is up first; an
out-of-range place goes home, as today.

CONFIG → Display → **Stock screens** clears `ui/active` (posts to `storage`),
so the pod can always get back to the stock layout without the hotspot. The
three quick key-ons still go home on whatever deck is showing.

### Who writes the deck

The live deck has **one writer: `render`**. Not `logic`, the usual settings
writer: `logic` is on core 0, and copying a 30 kB deck there is time taken
from the real-time island for nothing the real-time tasks need. Render is
on core 1 and is the only reader.

| Producer | Hands over | How |
|---|---|---|
| `storage` (card owner, core 1) | the active file, decoded and validated; or "stock" | a PSRAM mailbox: buffer + generation, written before the generation is bumped |
| `wifi` (core 1) | a draft from the editor, decoded and validated | its own PSRAM mailbox, same shape |
| `render` | takes the newest from either between frames, copies it into its deck, rebuilds `nav::Layout`, full repaint | the only writer of the live deck |

A draft beats the card while it is fresh. When the draft ends (saved,
cancelled, or 30 s with no edit), render goes back to whatever `storage`
last offered. `storage` re-reads `ui/active` on mount and when `wifi` posts that it has
written `ui/`. An unmount offers nothing, so render keeps what it has. Only
CONFIG → Stock screens or a card whose active file is gone or bad sends it
back to stock while running.

## 8. The editor (the web page)

A **Screens** tab in `kPage`, plain JavaScript like the rest of the page (no
framework, no CDN: the phone is on the dash's hotspot, with no internet).
Laid out the way Lopaka is:

```
+----------------------------------------------------------------------+
| SCREENS  [Mode: CRUISE v] [Screen: ENDURO v]  [+]  zoom 1x 2x 3x  grid |
+------+-------------------------------------------------+-------------+
| TOOLS|                                                 | LAYERS      |
| [ ] select                                             |  # readouts |
| [/] line         480 x 320 pixel canvas                |   SPEED  big|
| [□] rect         (checker outside the glass,           |   GEAR  val |
| [▢] round rect    pixel grid from 3x)                  |  # chrome   |
| [○] circle                                             |   panel     |
| [●] disc                                               |   rule      |
| [◠] arc                                                +-------------+
| [△] triangle                                           | PROPERTIES  |
| [T] text                                               | x  y  w  h  |
| [★] icon                                               | colour role |
| [✎] paint (1-bit image)                                | font  scale |
| ---- readouts ----                                     | field  [..] |
| [#] value  [8] big  [▬] bar  [◔] gauge  [•] lamp ...   | lo hi warn  |
+------+-------------------------------------------------+-------------+
| chrome 41/512 readouts 9/192  ✓ valid  [● Live on dash] [Save as...] |
+----------------------------------------------------------------------+
```

- **Canvas.** Pixel coordinates, snapping to 1 px or an optional 4/8 px
  grid. Drag to draw, drag to move, corner handles to resize. **Pointer
  Events**, so mouse and touch both work (HTML5 drag-and-drop does not fire
  on phones, and the page is used from a phone at the bike). Arrow keys
  nudge 1 px, Shift+arrow 10 px; Delete; Ctrl+D duplicate; Ctrl+Z / Ctrl+Y
  undo and redo; a two-finger pinch zooms.
- **Same pixels as the dash where it is cheap.** The 5×7 font
  (`font5x7.cpp`) and the icon bitmaps go into the page as small arrays,
  generated from the same sources by a tool, so text and icons preview
  pixel for pixel. Shapes and anti-aliased fonts are drawn with the
  browser's canvas and are close, not exact.
- **Colours** are a picker of theme roles (with the Standard/Contrast
  palette toggle to see both), plus a custom RGB565 swatch.
- **Field picker.** A search box over `/api/fields`: type `psi` or `lean`
  and pick. Grouped by sub-struct, showing unit and type. Choosing a field
  suggests a style (a bool → Lamp, a string → Text, `_ms` → Duration).
- **Live values.** While a screen is open the page polls
  `GET /api/values?ids=…` (2 Hz) for just the fields on the canvas, so the
  editor's canvas shows real numbers from the bike, not samples.
- **Live on the dash.** With *Live* on, every edit (debounced to ~300 ms)
  sends the whole deck to `PUT /api/draft`; the dash validates it and shows
  it at once, on the screen being edited. A refused draft leaves the dash on
  the last good one and the page shows why. The draft is never written to
  the card; it ends on Save, on turning Live off, after 30 s without an
  edit, or at key-off, and the dash goes back to the card's layout.
- **Layers.** Readouts are always drawn over chrome (they are widgets), so
  the list has two groups. Inside each group the order is the draw order:
  reorder, hide, lock, rename.
- **Modes view.** Modes as cards with their screens as rows: ↑ ↓ buttons,
  "move to mode", remove, add (a built-in not in use, or a custom screen).
  DIAG and CONFIG are shown greyed at the end.
- **Layouts.** A list of the card's `ui/*.dui`: open, save, save as,
  rename, delete, download, upload, and *Use this one* (writes
  `ui/active`). A "Stock" entry opens `default_deck()` (from the firmware,
  read-only) as a starting point for a new file.

New endpoints: `GET /api/fields`, `GET /api/values`, `PUT /api/draft`,
`GET /api/ui` (the files and the active one), `GET/PUT/DELETE /ui/*`,
`POST /api/ui/active`, `GET /api/ui/stock` (the default deck, encoded).
Eight more handlers: `max_uri_handlers` goes from 16 to 24.

**Page size.** The editor is maybe 25–35 kB of JavaScript beside the
existing 33 kB page. Flash has room. To keep the phone's load quick, the
page is gzipped at build time and sent with `Content-Encoding: gzip`.

---

## 9. Phases

Each phase builds, passes host tests and leaves the bike usable.

1. **Fields.** `tools/frame_fields.py`, the override file, the id registry,
   the generated table, the `--check` ctest. Nothing uses it yet.
2. **`core/layout`**: Deck, `default_deck()`, encode, decode, validate,
   `test_layout.cpp`.
3. **nav and App read the deck.** No visible change with `default_deck()`.
4. **`ui::CustomScreen`**: chrome, readouts, `ground` on widgets, the
   gauge and lamp widgets. The sim loads a deck file, so it can be checked
   without the board (features §29).
5. **The card**: `ui/` files, `storage` loading the active one on mount
   and dropping to stock on unmount, the mailboxes, render as the deck's
   writer, the splash hold, CONFIG → Stock screens. Host-tested with the
   fake card.
6. **The editor**, in a standalone `tools/editor/` HTML file first, run in a
   browser against the sim (or saved JSON files), then folded into `kPage`
   with the endpoints.
7. **Live editing**: `/api/values`, `/api/draft`, the draft timeout.
8. **Docs and version.** features.md (a new section, plus §3, §10, §25),
   system-architecture.md (the deck's owner, the field table),
   `firmware.txt`.

Phases 1–4 and most of 6 need no board.

---

## 10. Risks and traps

- **Chrome under a readout** gets wiped on the first update (§1). It is
  refused by validation, not left to the rider to discover.
- **Render stack.** `CustomScreen` must not be the largest screen.
- **Internal RAM.** The live deck and the two mailboxes (three × ~30 kB)
  go in PSRAM (`EXT_RAM_BSS_ATTR`), never a plain static.
- **A loose card is not a layout change.** The deck is in PSRAM once read,
  so a card that drops out over bumps changes nothing on the glass (§7).
- **Bus budget.** A full frame is ~12 ms whatever is on it. A screen of 16
  readouts that all change every frame (a sparkline, rpm, lean ...) can cost
  more than that per frame. The editor shows each readout's worst-case
  pixel area, and the console's wire-cost report (`widget_name`) names
  readouts by their field, so a slow screen can be found.
- **Reading the Frame from `wifi`.** `/api/values` takes a
  `core::state::snapshot()` on core 1. That reads only, so the one-writer
  rule holds; it costs one Frame copy per poll.
- **Blackbox unaffected.** A deck changes what is drawn, never what is
  logged; the column list stays fixed.
- **Ids are permanent**, both field ids and `ScreenId` numbers.
- **Header parsing.** The generator handles the C++ the Frame actually
  uses (plain members, nested structs, fixed arrays, enums) and **fails
  loudly** on anything else, rather than skipping a field quietly.
- **No flash writes at all** for layouts: the card holds them. The
  hotspot is off while moving, so the page cannot change a layout mid-ride.

## 11. Decisions

Settled (2026-11-14):

- **Card pulled mid-ride: keep the loaded layout until key-off.** Stock only
  at a boot that finds no card or no good active file.
- **DIAG and CONFIG pinned** as the last two modes, outside any layout.
- **Short E on a custom screen runs one CONFIG action**, with CONFIG's
  confirm rule. Long E stays the overlay menu.
- **A screen may appear in several modes**, built-in or custom.
- Pixel placement with optional snapping, Lopaka's way.
- Built-in screens stay code: moved, hidden and repeated, not edited
  element by element. A "start from SPEED" template is cheap; editable
  built-ins are not.
- Series (maps, lap lists, the heart-rate graph, route pace) stay on their
  built-in screens; not readouts.
- The pod does not edit layouts; it can only go back to stock.
- No screenshots over Wi-Fi. The console's `screen` over USB is unchanged.

Still open, none blocking the first phases:

- **Maxima**: 8 riding modes, 12 screens per mode, 16 custom screens, 16
  readouts and 2 sparklines per screen, 512 chrome elements and 192
  readouts across the deck. Changeable until the first `.dui` ships; after
  that, only with a format version bump.
- **Draft timeout** of 30 s without an edit: long enough to walk from the
  phone to the bike and look?
- Whether E may also flip a CONFIG **On/Off** item (Notifications, Slip
  bar), not only run actions. The same few lines; say if wanted.

## 12. As built (2.23)

- **One readout widget.** `ui::ReadoutWidget` draws every style itself
  rather than wrapping `ValueBox`, `BigNumber` and `Bar`: one class makes a
  screen a plain array (no heap), and gives every style its `ground`
  colour. The cost: `Big` repaints its whole box, not only the digits that
  changed.
- **Fonts.** The 5×7 font at a scale; no font choice per element.
- **The trace** keeps samples as bytes (0..254 of lo..hi), 120 at 10 Hz:
  floats made `CustomScreen` exactly as big as the largest screen
  (`PerfScreen`, 2832 bytes); it is 2104 now. `test_custom` checks it never
  becomes the largest.
- **Who writes the deck.** `render` (§7) through `services::LayoutFollower`;
  `storage` and `wifi` hand decks over in single-slot `DeckHandoff`s.
- **Boot.** Render does not wait for the card: it starts on stock and the
  card's layout arrives a pass of `storage` later; inside 5 s of boot it
  keeps the place the dash was left on (with the splash on, behind it).
- **Images** are imported from a PNG (thresholded, up to 64×64) rather
  than painted pixel by pixel.
- **No JSON export**: the `.dui` file is the backup (Download / Upload).

