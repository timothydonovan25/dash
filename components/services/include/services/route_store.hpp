// Routing between `logic` and the card (core::route, docs/navigation.md §6).
//
// The map is on the card and `logic` is on core 0, where nothing touches
// the filesystem; `storage` owns the card on core 1. As with saved tracks
// (track_store.hpp), this is a one-job mailbox: `logic` posts while it is
// Free, `storage` answers, `logic` reads and frees. Nothing on core 0 ever
// waits for the card.
//
// The route itself is double-buffered: `logic` follows `data[live]` while
// `storage` writes a new route into the other buffer. `live` is `logic`'s
// and changes only after an answer, when `storage` no longer holds the
// turn, so the two never touch one buffer at once.
//
// Nothing here includes an ESP header: the host tests and the simulator
// serve it from a card in RAM.
#pragma once
#include <atomic>
#include <cstdint>
#include "core/route/dest.hpp"
#include "core/route/navigator.hpp"
#include "core/route/route.hpp"
#include "core/route/search.hpp"
#include "core/state/track_buffer.hpp"
#include "core/state/vehicle.hpp"
#include "hal/i_storage.hpp"

namespace services {

inline constexpr const char* kMapPath  = "route/map.rgf";
inline constexpr const char* kDestPath = "route/destinations.csv";
inline constexpr const char* kSearchPath = "route/search.idx";
inline constexpr const char* kRecentPath = "route/recent.csv";

// A statewide route is ~4,000 points on average in the Arkansas build;
// these leave room for the longest.
inline constexpr uint32_t kRoutePts  = 32768;
inline constexpr uint32_t kRouteCues = 1024;
inline constexpr size_t   kRoadName  = 24;

struct RouteData {
    core::route::RoutePoint pts[kRoutePts];
    core::route::Cue        cues[kRouteCues];
    char                    cue_names[kRouteCues][kRoadName];
    core::route::RoadLabel  labels[core::state::Nav::kMaxLabels];
    char                    label_names[core::state::Nav::kMaxLabels][kRoadName];
    uint8_t                 n_labels = 0;
    core::route::Route      route{pts, kRoutePts, cues, kRouteCues};
};

// The live route for CRUISE -> MAP, in the trip trace's frame (feet from
// the session origin), thinned to fit. One writer, `logic`, which fills it
// when it adopts a route and empties it when it stops following one; `gen`
// moves each time, which is how the map knows to redraw.
struct RouteTrace {
    static constexpr size_t kPoints = 2048;
    core::state::TrackBuffer<core::state::TrackPoint, kPoints> pts;
    std::atomic<uint32_t> gen{0};
};

struct RouteMailbox {
    enum class Turn : uint8_t { Free = 0, Posted, Answered };
    enum class Op : uint8_t {
        None = 0,
        Open,          // look at the map file: `map_ok`, `map_date`
        Destinations,  // favourites and the POIs nearest `near`: `dests`
        Query,         // `req` into `data[into]`: `status`, `stats`
        Search,        // "Where to?": `query` near `near`: `next`, `matches`, `hits`
        House,         // number `house_number` on street `house_entry`: `house_at`
        SaveRecent,    // put `recent` first in route/recent.csv: `ok`
    };
    static constexpr size_t kMaxHits = 8;
    static constexpr size_t kMaxDests = 10;

    std::atomic<Turn> turn{Turn::Free};
    Op op = Op::None;

    // Open's answer.
    bool     map_ok = false;
    uint32_t map_date = 0;
    // Destinations.
    core::route::Point near{};
    core::route::Dest  dests[kMaxDests];
    uint8_t            n_dests = 0;
    // Search (core/route/search.hpp). `search_ok` is whether the card has
    // an index; results are listed once the query is 3 letters or the
    // matches are few, so a one-letter search does not read half of it.
    char                    query[core::route::kKeyLen + 1] = {};
    bool                    search_ok = false;
    uint64_t                next = 0;
    uint32_t                matches = 0;
    core::route::SearchHit  hits[kMaxHits];
    uint8_t                 n_hits = 0;
    // SaveRecent.
    core::route::Dest       recent{};
    bool                    ok = false;
    // House.
    uint32_t                house_entry = 0;
    uint32_t                house_number = 0;
    core::route::Point      house_at{};
    bool                    house_exact = false;
    // Query.
    core::route::Request    req{};
    uint8_t                 into = 0;
    core::route::Status     status = core::route::Status::Ok;
    core::route::QueryStats stats{};

    RouteData data[2];
    uint8_t   live = 0xFF;       // `logic`'s: the buffer being followed, 0xFF none

    bool free() const { return turn.load(std::memory_order_acquire) == Turn::Free; }
    void post() { turn.store(Turn::Posted, std::memory_order_release); }
    bool answered() const { return turn.load(std::memory_order_acquire) == Turn::Answered; }
    void release() { turn.store(Turn::Free, std::memory_order_release); }
};

// The card as a block source for core::route. The SD driver keeps the
// file open between reads (SdCard::Reader): an fopen per 4 kB block made a
// 181 mi route 28 s on the bike; held open with fast seek, 4.5 s.
class CardBlockSource final : public core::route::IBlockSource {
public:
    explicit CardBlockSource(const char* path) : path_(path) {}
    void set(hal::ICard* card, uint32_t size) { card_ = card; size_ = size; }
    bool read(uint32_t offset, void* dst, uint32_t len) override;
    uint32_t size() const override { return size_; }

private:
    const char* path_;
    hal::ICard* card_ = nullptr;
    uint32_t size_ = 0;
};

// `storage`'s routing memory: the block caches and the route search. In
// PSRAM on the board (~5.5 MB).
struct RouteEngine {
    core::route::BlockCacheStorage<1024> cache_mem;    // 4 MB
    core::route::SearchStorage<8192, 4096> search;     // a route settles ~250 nodes, 416 at most
    core::route::BlockCache cache = cache_mem.make();
    core::route::Graph      graph{cache};
    CardBlockSource         src{kMapPath};
    char                    text[2048];         // destinations.csv or recent.csv, read whole
    core::route::Dest       recents[6];         // recent.csv, rewritten
    // "Where to?": its own file, so its own cache (1 MB).
    core::route::BlockCacheStorage<256> search_mem;
    core::route::BlockCache  search_cache = search_mem.make();
    core::route::SearchIndex index{search_cache};
    CardBlockSource          search_src{kSearchPath};
};

// `storage`'s side: does the posted job, if there is one. True if it did.
// A query blocks the calling task for its card reads (a second or two cold).
bool serve_route(RouteMailbox& box, RouteEngine& eng, hal::ICard& card);

}  // namespace services
