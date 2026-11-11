#include "services/route_store.hpp"
#include <cstdio>
#include <cstring>

namespace services {

using namespace core::route;

bool CardBlockSource::read(uint32_t offset, void* dst, uint32_t len) {
    if (!card_) return false;
    size_t got = 0;
    return hal::ok(card_->read_at(path_, offset, dst, len, got)) && got == len;
}

namespace {

// (Re)opens the map when the file on the card is not the one the graph
// has: a new card, or a map copied over or uploaded since.
bool ensure_open(RouteEngine& eng, hal::ICard& card) {
    size_t size = 0;
    if (!hal::ok(card.size_of(kMapPath, size)) || size < sizeof(Header) || size > 0xFFFFFFFFu) {
        eng.src.set(nullptr, 0);
        eng.cache.attach(nullptr);
        return false;
    }
    Header h;
    size_t got = 0;
    if (!hal::ok(card.read_at(kMapPath, 0, &h, sizeof h, got)) || got != sizeof h) return false;
    // The card handed in this time, always: it may be a wrapper that lives
    // only for this job (storage's ReadsKeepAlive), not the one the map was
    // opened with. Keeping the old one crashed `route bench` (2.8).
    const bool same = eng.graph.is_open() && eng.src.size() == size &&
                      std::memcmp(&h, &eng.graph.header(), sizeof h) == 0;
    eng.src.set(&card, uint32_t(size));
    if (same) return true;
    return eng.graph.open(eng.src) == OpenResult::Ok;
}

void name_into(Graph& g, uint32_t off, char* out) { g.name(off, out, kRoadName); }

void do_query(RouteMailbox& box, RouteEngine& eng) {
    RouteData& d = box.data[box.into & 1];
    box.stats = QueryStats{};
    box.status = query(eng.graph, box.req.variant_flags, box.req.from, box.req.to,
                       eng.search.mem(), d.route, &box.stats);
    d.route.variant_flags = box.req.variant_flags;
    if (box.status != Status::Ok) return;
    // The names `logic` and the screen need, read here where the card is.
    for (uint32_t i = 0; i < d.route.n_cues; ++i) name_into(eng.graph, d.route.cues[i].name_off, d.cue_names[i]);
    name_into(eng.graph, d.route.first_fast_name, d.fast_name);
    d.n_labels = uint8_t(longest_roads(d.route, d.labels, core::state::Nav::kMaxLabels));
    for (uint8_t i = 0; i < d.n_labels; ++i) name_into(eng.graph, d.labels[i].name_off, d.label_names[i]);
}

size_t read_dests(hal::ICard& card, const char* path, char* text, size_t cap, Dest* out, size_t max) {
    size_t got = 0;
    if (!hal::ok(card.read_at(path, 0, text, cap, got)) || got == 0) return 0;
    return parse_dests(text, got, out, max);
}

void do_destinations(RouteMailbox& box, RouteEngine& eng, hal::ICard& card) {
    box.n_dests = 0;
    // Favourites first: the rider wrote them, so they lead the list. Then
    // the places recently gone to, then the nearest fuel and food.
    box.n_dests = uint8_t(read_dests(card, kDestPath, eng.text, sizeof eng.text, box.dests, 3));
    const size_t r = read_dests(card, kRecentPath, eng.text, sizeof eng.text, box.dests + box.n_dests, 3);
    for (size_t i = 0; i < r; ++i) box.dests[box.n_dests + i].kind = kDestRecent;
    box.n_dests = uint8_t(box.n_dests + r);
    box.n_dests = uint8_t(box.n_dests + nearest_pois(eng.graph, box.near, PoiFuel,
                                                     box.dests + box.n_dests, 2));
    box.n_dests = uint8_t(box.n_dests + nearest_pois(eng.graph, box.near, PoiFood,
                                                     box.dests + box.n_dests, 2));
}

// The card, not flash: a write while riding stalls nothing on core 0.
void do_save_recent(RouteMailbox& box, RouteEngine& eng, hal::ICard& card) {
    Dest old[5];
    const size_t n_old = read_dests(card, kRecentPath, eng.text, sizeof eng.text, old, 5);
    const size_t n = push_recent(box.recent, old, n_old, eng.recents, 5);
    size_t len = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t k = format_dest_line(eng.recents[i], eng.text + len, sizeof eng.text - len);
        if (k == 0) break;
        len += k;
    }
    box.ok = hal::ok(card.write_file(kRecentPath, eng.text, len));
}

// The search index, (re)opened when the file on the card changes.
bool ensure_search(RouteEngine& eng, hal::ICard& card) {
    size_t size = 0;
    if (!hal::ok(card.size_of(kSearchPath, size)) || size < sizeof(SearchHeader) || size > 0xFFFFFFFFu) {
        eng.search_src.set(nullptr, 0);
        eng.search_cache.attach(nullptr);
        return false;
    }
    SearchHeader h;
    size_t got = 0;
    if (!hal::ok(card.read_at(kSearchPath, 0, &h, sizeof h, got)) || got != sizeof h) return false;
    const bool same = eng.index.is_open() && eng.search_src.size() == size &&
                      std::memcmp(&h, &eng.index.header(), sizeof h) == 0;
    eng.search_src.set(&card, uint32_t(size));   // this job's card, as ensure_open
    if (same) return true;
    return eng.index.open(eng.search_src);
}

// Results only once they are worth reading: a 3-letter query, or few
// enough matches. One letter of a statewide index is thousands of keys.
constexpr size_t   kListFromLetters = 3;
constexpr uint32_t kListUnder = 300;

void do_search(RouteMailbox& box, RouteEngine& eng, hal::ICard& card) {
    box.n_hits = 0;
    box.matches = 0;
    box.next = 0;
    box.search_ok = ensure_search(eng, card);
    if (!box.search_ok) return;
    box.query[kKeyLen] = '\0';
    SearchIndex& idx = eng.index;
    // "13511 RYAN": the number is a house, the rest spells the street, and
    // the results are that house on each street found (a Garmin's order).
    uint32_t house = 0;
    const char* street = nullptr;
    const bool numbered = split_house(box.query, house, street);
    const char* q = numbered ? street : box.query;
    box.next = idx.next_chars(q);
    // Still typing a number: it may be a house, so digits and a space stay.
    bool digits = box.query[0] != '\0';
    for (const char* c = box.query; *c && digits; ++c) digits = *c >= '0' && *c <= '9';
    if (digits && std::strlen(box.query) < 6) {
        for (const char* c = "0123456789 "; *c; ++c)
            box.next |= uint64_t(1) << (std::strchr(kAlphabet, *c) - kAlphabet);
    }
    // 3+ digits is a house number: the street's first letter can come next
    // (logic puts the space in).
    if (digits && std::strlen(box.query) >= 3) box.next |= idx.next_chars("");
    uint32_t first = 0, last = 0;
    if (!idx.range(q, first, last)) return;
    box.matches = last - first;
    if (std::strlen(q) < kListFromLetters && box.matches > kListUnder) return;
    if (numbered && !*q) return;                         // a number, and no street yet
    const size_t n = idx.find(q, box.near, box.hits, RouteMailbox::kMaxHits);
    if (!numbered) {
        box.n_hits = uint8_t(n);
        return;
    }
    // The streets, best fit for the number first: one OSM has the house
    // on, then one whose numbers run past it, then the rest; nearest
    // first within each. (Two Ryan Rds a mile apart: the one with 13511.)
    uint8_t fits[RouteMailbox::kMaxHits];
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        SearchHit h = box.hits[i];
        if (h.kind != Place::Street) continue;
        Point p;
        bool exact = false;
        uint8_t fit = 3;
        if (!idx.house(h.entry, house, p, exact, &fit)) continue;
        h.p = p;
        h.dist_m = uint32_t(dist_m(box.near, p));
        char label[sizeof h.name];
        std::snprintf(label, sizeof label, "%u %.16s", unsigned(house), h.name);   // cut to fit
        std::memcpy(h.name, label, sizeof h.name);
        h.nums_n = 0;                // the number is chosen: it goes straight there
        size_t at = k++;             // insertion by (fit, distance); the hits came nearest first
        while (at > 0 && fits[at - 1] > fit) {
            box.hits[at] = box.hits[at - 1];
            fits[at] = fits[at - 1];
            --at;
        }
        box.hits[at] = h;
        fits[at] = fit;
    }
    box.n_hits = uint8_t(k);
}

void do_house(RouteMailbox& box, RouteEngine& eng, hal::ICard& card) {
    box.search_ok = ensure_search(eng, card);
    box.house_exact = false;
    if (!box.search_ok || !eng.index.house(box.house_entry, box.house_number, box.house_at, box.house_exact))
        box.house_at = Point{};
}

}  // namespace

bool serve_route(RouteMailbox& box, RouteEngine& eng, hal::ICard& card) {
    if (box.turn.load(std::memory_order_acquire) != RouteMailbox::Turn::Posted) return false;
    const bool open = ensure_open(eng, card);
    box.map_ok = open;
    box.map_date = open ? eng.graph.header().osm_date : 0;
    switch (box.op) {
        case RouteMailbox::Op::Query:
            if (open) do_query(box, eng);
            else box.status = Status::NoMap;
            break;
        case RouteMailbox::Op::Destinations:
            if (open) do_destinations(box, eng, card);
            else box.n_dests = 0;
            break;
        case RouteMailbox::Op::Search:
            do_search(box, eng, card);
            break;
        case RouteMailbox::Op::House:
            do_house(box, eng, card);
            break;
        case RouteMailbox::Op::SaveRecent:
            do_save_recent(box, eng, card);
            break;
        case RouteMailbox::Op::Open:
        case RouteMailbox::Op::None:
            break;
    }
    box.turn.store(RouteMailbox::Turn::Answered, std::memory_order_release);
    return true;
}

}  // namespace services
