// Custom layouts on the card, and how they reach `render`
// (docs/custom-screens.md §7). Board-free: `storage`, `wifi`, the simulator
// and test_layout_store all use it.
//
//   ui/active        one line: the .dui in use; missing or empty = stock
//   ui/<name>.dui    core::layout's encoded decks
//
// The stock layout is compiled in. No card, no active file, or one that
// does not decode: the dash shows stock. A card pulled while running changes
// nothing until key-off, because the deck render holds is its own copy.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "core/layout/deck.hpp"
#include "hal/i_storage.hpp"

namespace services {

inline constexpr const char* kUiDir        = "ui";
inline constexpr const char* kUiActivePath = "ui/active";
inline constexpr size_t      kUiNameLen    = 24;   // "<16 chars>.dui" and a NUL
// How long a live draft from the editor stays up without another edit.
inline constexpr uint32_t    kDraftIdleMs  = 30000;

// "enduro.dui": [a-z0-9-]{1,16} then ".dui". The page's names are checked
// with this before any path is built from them.
bool valid_ui_name(const char* name);

// One deck handed from a producer to `render`. A single slot: the producer
// writes only when render has taken the last one (free()), and render marks
// it taken only after copying it out, so neither side ever sees half a deck.
struct DeckHandoff {
    core::layout::Deck    deck;
    bool                  stock = false;    // no deck: the compiled-in layout
    uint16_t              place = 0;        // draft: (mode << 8) | screen to show
    std::atomic<uint32_t> posted{0};
    std::atomic<uint32_t> taken{0};

    bool free() const {
        return posted.load(std::memory_order_acquire) == taken.load(std::memory_order_acquire);
    }
    bool waiting() const { return !free(); }
    void post() { posted.fetch_add(1, std::memory_order_release); }
    void take() { taken.store(posted.load(std::memory_order_acquire), std::memory_order_release); }
};

struct LayoutMailbox {
    DeckHandoff card;    // storage -> render: the card's active layout, or stock
    DeckHandoff draft;   // wifi -> render: the editor's live draft
    // wifi -> storage: ui/ changed (or a draft ended), read the active one again.
    std::atomic<bool> reload{false};
    // logic -> storage: CONFIG -> Stock screens. Clear ui/active and reload.
    std::atomic<bool> stock_req{false};
    // wifi -> render: the draft is over (saved, or Live turned off).
    std::atomic<bool> draft_end{false};
    // storage's last read, for the page. One writer (storage); a read can
    // land mid-copy and show a torn name for one poll, which is all it costs.
    char active[kUiNameLen] = {};
    char error[48] = {};
    // storage's alone: a read is owed, waiting for render to take the last.
    bool pending = false;
    // storage's scratch for a file's bytes.
    uint8_t buf[core::layout::kMaxEncoded];
};

enum class LoadResult : uint8_t { Stock, Loaded, Refused };

// Reads ui/active and the file it names into `out`. `name` gets the file
// name ("" for stock), `why` the reason on Refused.
LoadResult load_active(hal::ICard& card, uint8_t* buf, size_t cap,
                       core::layout::Deck& out, char* name, size_t name_n,
                       core::layout::Error* why);

// A whole file into ui/, through a .tmp renamed over the old one, so a card
// pulled mid-write leaves the old file whole.
hal::Status write_ui_file(hal::ICard& card, const char* name,
                          const uint8_t* data, size_t n);
// ui/active names `name`; "" for stock.
hal::Status set_active(hal::ICard& card, const char* name);

// `render`'s half: takes what the producers posted into its own `live`
// copy and says when the App must change deck. A draft from the editor wins
// while it is fresh; when it ends, storage is asked for the card's again.
class LayoutFollower {
public:
    struct Change {
        bool                      apply = false;
        const core::layout::Deck* deck = nullptr;   // null: stock
        uint16_t                  place = 0;        // App::set_deck's
    };
    // The card's first answer within this long of boot keeps the place the
    // dash was left on (`boot_place`); later ones go home.
    static constexpr uint32_t kBootWindowMs = 5000;
    Change step(LayoutMailbox& box, core::layout::Deck& live, uint32_t now_ms,
                uint16_t boot_place);
    bool draft_on() const { return draft_on_; }

private:
    bool     draft_on_ = false;
    uint32_t draft_ms_ = 0;
    bool     answered_ = false;     // the card has answered once
    bool     showing_stock_ = true;
};

// `storage`'s pass: on a fresh mount or a reload request, read the active
// layout and post it to render. Waits (returns, to be called again next
// pass) while render has not taken the last one.
void serve_layout(LayoutMailbox& box, hal::ICard& card, bool mounted_now);

}  // namespace services
