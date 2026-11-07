// Address and place search on the card (docs/navigation.md §7.1): the
// dash's "Where to?", spelled with the pod like an old Garmin.
//
// route/search.idx holds every named street (with its town), the house
// numbers OSM has on each, towns and villages, and named places (fuel,
// food, shops...). Every entry is reachable by the start of any of its
// words: "COLLEGE" finds "N College Ave". The keys are sorted, so a prefix
// is a binary search; the letters that can come next are found the same
// way, which is what lets the keyboard offer only letters that lead
// somewhere.
//
// Read through a BlockCache like the map. No allocation, no ESP header.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/route/graph.hpp"

namespace core::route {

inline constexpr char     kSearchMagic[4] = {'R', 'S', 'I', '1'};
inline constexpr uint16_t kSearchVersion  = 1;
inline constexpr size_t   kKeyLen         = 24;   // a key, NUL padded

// What an entry is.
enum class Place : uint8_t { Street = 1, Town = 2, Poi = 3 };

struct SearchHeader {
    char     magic[4];
    uint16_t version;
    uint16_t pad;
    uint32_t osm_date;
    uint32_t entry_count;
    uint32_t entries_off;
    uint32_t num_count;
    uint32_t nums_off;
    uint32_t key_count;
    uint32_t keys_off;
    uint32_t names_off;
    uint32_t names_bytes;
};
static_assert(sizeof(SearchHeader) == 44, "on-card layout");

struct PlaceEntry {
    Place    kind;
    uint8_t  sub;          // a POI's PoiKind (0 other); a town's size (3 city .. 0 hamlet)
    uint16_t pad;
    uint32_t name_off;     // display name, into names
    uint32_t city_off;     // its town, into names; 0 is ""
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint32_t nums_off;     // a street's house numbers, sorted by number
    uint32_t nums_n;
};
static_assert(sizeof(PlaceEntry) == 28, "on-card layout");

struct HouseNumber {
    uint32_t number;
    int32_t  lat_e7;
    int32_t  lon_e7;
};
static_assert(sizeof(HouseNumber) == 12, "on-card layout");

// The entry's point rides in its keys, so ranking by distance reads only
// the keys, in order, and the entries of the winners.
struct SearchKey {
    char     key[kKeyLen];
    uint32_t entry;        // | kFullKey on the key that is the whole name
    int32_t  lat_e7;
    int32_t  lon_e7;
};
static_assert(sizeof(SearchKey) == 36, "on-card layout");
inline constexpr uint32_t kFullKey = 1u << 31;
inline constexpr uint32_t kTownKey = 1u << 30;    // the entry is a town
inline constexpr uint32_t kEntryMask = kTownKey - 1;

// What can be typed, in keyboard order: a letter, a digit or a space.
inline constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
inline constexpr size_t kAlphabetLen = sizeof(kAlphabet) - 1;   // 37

// The search form of a name: ASCII letters upper-cased, digits kept,
// anything else one space, no leading, trailing or double spaces; cut to
// `cap - 1`. The builder and the keyboard both use it, so they agree.
size_t normalize(const char* in, char* out, size_t cap);

// The word starts of a normalized name an entry is found by: the whole
// name, then from each later word that is not a compass point or a street
// type ("N COLLEGE AVE" -> "N COLLEGE AVE", "COLLEGE AVE"). Calls fn(key).
template <typename Fn>
void each_key(const char* norm, Fn&& fn);
bool is_filler_word(const char* w, size_t n);

// "13511 RYAN": a house number typed first, as on a Garmin, then the
// street. True when `q` starts with 1 to 6 digits and a space; `number`
// and `street` (the rest, maybe "") are then set.
bool split_house(const char* q, uint32_t& number, const char*& street);

struct SearchHit {
    uint32_t entry = kNone;
    Place    kind = Place::Street;
    uint8_t  sub = 0;
    uint32_t nums_n = 0;
    Point    p{};
    uint32_t dist_m = 0;
    bool     whole = false;    // the search is the whole name
    char     name[24] = {};
    char     city[16] = {};
};

class SearchIndex {
public:
    explicit SearchIndex(BlockCache& cache) : cache_(cache) {}

    // False for a missing, foreign or damaged file.
    bool open(IBlockSource& src);
    bool is_open() const { return open_; }
    const SearchHeader& header() const { return h_; }

    // The keys starting with `prefix` (normalized): [first, last).
    bool range(const char* prefix, uint32_t& first, uint32_t& last);

    // Which of kAlphabet can follow `prefix` and still match something:
    // bit i for kAlphabet[i]. 0 when nothing follows (or on a read error).
    uint64_t next_chars(const char* prefix);

    // The `max` entries nearest `near` among those matching `prefix`,
    // nearest first; each entry once however many of its keys match. A
    // town the search is the whole name of ("LITTLE ROCK", the city) comes
    // before the rest ("Little Rock Rd", 2 mi away). For anything else
    // nearest wins: "WALMART" is the nearest Walmart, whatever its full name.
    // Looks at no more than `scan` keys, so a one-letter search on a big
    // index stays quick (the nearest of the first `scan` then).
    // `*matches` gets how many keys matched. Returns the count found.
    size_t find(const char* prefix, Point near, SearchHit* out, size_t max,
                uint32_t* matches = nullptr, uint32_t scan = 2048);

    // A street's house `number`: the point OSM has for it, or one placed
    // between the nearest numbers either side on that street (`exact`
    // false), or the street's own point when it has none. False on a read
    // error or a bad entry.
    // `fit`, when given: how well the number fits the street: 0 OSM has it,
    // 1 between two it has, 2 past the ends of its numbers, 3 it has none.
    bool house(uint32_t entry, uint32_t number, Point& out, bool& exact, uint8_t* fit = nullptr);

    bool entry(uint32_t i, PlaceEntry& out);
    void name(uint32_t off, char* out, size_t cap);

private:
    bool key(uint32_t i, SearchKey& out);
    // First key >= prefix (by its first strlen(prefix) bytes).
    bool lower(const char* prefix, size_t n, bool upper, uint32_t& out);

    BlockCache& cache_;
    SearchHeader h_{};
    bool open_ = false;
};

template <typename Fn>
void each_key(const char* norm, Fn&& fn) {
    fn(norm);
    for (const char* p = norm; *p; ++p) {
        if (*p != ' ') continue;
        const char* w = p + 1;
        size_t n = 0;
        while (w[n] && w[n] != ' ') ++n;
        if (n && !is_filler_word(w, n)) fn(w);
    }
}

}  // namespace core::route
