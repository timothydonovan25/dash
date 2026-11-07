#include "core/route/search.hpp"
#include <cstring>
#include "core/route/route.hpp"

namespace core::route {

size_t normalize(const char* in, char* out, size_t cap) {
    if (cap == 0) return 0;
    size_t n = 0;
    bool space = false;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!keep) {
            space = n > 0;
            continue;
        }
        if (space && n + 2 < cap) out[n++] = ' ';
        space = false;
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

bool is_filler_word(const char* w, size_t n) {
    static const char* const kWords[] = {
        "N", "S", "E", "W", "NE", "NW", "SE", "SW", "NORTH", "SOUTH", "EAST", "WEST",
        "ST", "STREET", "AVE", "AVENUE", "RD", "ROAD", "DR", "DRIVE", "LN", "LANE", "BLVD",
        "CT", "COURT", "PL", "PLACE", "WAY", "HWY", "HIGHWAY", "PKWY", "CIR", "TRL", "TER",
        "LOOP", "THE", "OF", "AND", "US", "AR", "CR", "COUNTY",
    };
    for (const char* k : kWords)
        if (std::strlen(k) == n && std::memcmp(k, w, n) == 0) return true;
    return false;
}

bool split_house(const char* q, uint32_t& number, const char*& street) {
    size_t n = 0;
    uint32_t v = 0;
    while (q[n] >= '0' && q[n] <= '9') {
        if (++n > 6) return false;
        v = v * 10 + uint32_t(q[n - 1] - '0');
    }
    if (n == 0 || q[n] != ' ' || v == 0) return false;
    number = v;
    street = q + n + 1;
    return true;
}

bool SearchIndex::open(IBlockSource& src) {
    open_ = false;
    cache_.attach(&src);
    if (!cache_.read(0, &h_, sizeof h_)) return false;
    if (std::memcmp(h_.magic, kSearchMagic, 4) != 0 || h_.version != kSearchVersion) return false;
    const uint64_t size = src.size();
    auto fits = [&](uint32_t off, uint64_t count, uint64_t each) { return off + count * each <= size; };
    if (!fits(h_.entries_off, h_.entry_count, sizeof(PlaceEntry)) ||
        !fits(h_.nums_off, h_.num_count, sizeof(HouseNumber)) ||
        !fits(h_.keys_off, h_.key_count, sizeof(SearchKey)) || !fits(h_.names_off, h_.names_bytes, 1))
        return false;
    open_ = true;
    return true;
}

bool SearchIndex::key(uint32_t i, SearchKey& out) {
    return i < h_.key_count && cache_.read(h_.keys_off + i * uint32_t(sizeof(SearchKey)), &out, sizeof out);
}

bool SearchIndex::entry(uint32_t i, PlaceEntry& out) {
    return i < h_.entry_count &&
           cache_.read(h_.entries_off + i * uint32_t(sizeof(PlaceEntry)), &out, sizeof out);
}

void SearchIndex::name(uint32_t off, char* out, size_t cap) {
    if (cap == 0) return;
    size_t n = 0;
    while (n + 1 < cap && off + n < h_.names_bytes) {
        char c;
        if (!cache_.read(h_.names_off + off + uint32_t(n), &c, 1) || c == '\0') break;
        out[n++] = c;
    }
    out[n] = '\0';
}

bool SearchIndex::lower(const char* prefix, size_t n, bool upper, uint32_t& out) {
    uint32_t lo = 0, hi = h_.key_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        SearchKey k;
        if (!key(mid, k)) return false;
        const int c = std::memcmp(k.key, prefix, n);
        if (c < 0 || (upper && c == 0)) lo = mid + 1;
        else hi = mid;
    }
    out = lo;
    return true;
}

bool SearchIndex::range(const char* prefix, uint32_t& first, uint32_t& last) {
    if (!open_) return false;
    char p[kKeyLen + 1];
    const size_t n = normalize(prefix, p, sizeof p) ;
    // normalize() drops a trailing space; one typed as a word break counts.
    size_t len = n;
    const size_t raw = std::strlen(prefix);
    if (raw && prefix[raw - 1] == ' ' && len && len < kKeyLen) p[len++] = ' ';
    if (len == 0) { first = 0; last = h_.key_count; return true; }
    return lower(p, len, false, first) && lower(p, len, true, last);
}

uint64_t SearchIndex::next_chars(const char* prefix) {
    uint64_t mask = 0;
    char p[kKeyLen + 2];
    const size_t n = std::strlen(prefix);
    if (n + 1 > kKeyLen) return 0;
    std::memcpy(p, prefix, n);
    for (size_t i = 0; i < kAlphabetLen; ++i) {
        if (kAlphabet[i] == ' ' && (n == 0 || prefix[n - 1] == ' ')) continue;   // no double or leading space
        p[n] = kAlphabet[i];
        p[n + 1] = '\0';
        uint32_t a, b;
        if (!range(p, a, b)) return 0;
        if (a < b) mask |= uint64_t(1) << i;
    }
    return mask;
}

size_t SearchIndex::find(const char* prefix, Point near, SearchHit* out, size_t max,
                         uint32_t* matches, uint32_t scan) {
    uint32_t first, last;
    if (matches) *matches = 0;
    if (!range(prefix, first, last)) return 0;
    if (matches) *matches = last - first;
    char q[kKeyLen + 1];
    const size_t qn = normalize(prefix, q, sizeof q);
    // Whole-name matches first, then by distance.
    const auto before = [](const SearchHit& a, bool whole, uint32_t d) {
        return a.whole != whole ? whole : d < a.dist_m;
    };
    size_t n = 0;
    const uint32_t end = last - first > scan ? first + scan : last;
    for (uint32_t i = first; i < end; ++i) {
        SearchKey k;
        if (!key(i, k)) return 0;
        const uint32_t ent = k.entry & kEntryMask;
        // Whole: a town the search is all of the name of, not the tail of
        // a longer one ("Bank of Little Rock" is not the city).
        const bool whole = (k.entry & kFullKey) && (k.entry & kTownKey) &&
                           (qn == kKeyLen || k.key[qn] == '\0');
        size_t seen = n;
        for (size_t j = 0; j < n && seen == n; ++j) if (out[j].entry == ent) seen = j;
        if (seen != n) {
            if (!whole || out[seen].whole) continue;
            // Its full-name key, after a tail key: promote it.
            for (size_t j = seen; j + 1 < n; ++j) out[j] = out[j + 1];
            --n;
        }
        const Point p{k.lat_e7, k.lon_e7};
        const uint32_t d = uint32_t(dist_m(near, p));
        if (n == max && !before(out[n - 1], whole, d)) continue;
        size_t at = n < max ? n++ : n - 1;          // insertion, best first
        while (at > 0 && before(out[at - 1], whole, d)) { out[at] = out[at - 1]; --at; }
        SearchHit& h = out[at];
        h = SearchHit{};
        h.entry = ent;
        h.p = p;
        h.dist_m = d;
        h.whole = whole;
    }
    // The rest only for the winners: a read or two each.
    for (size_t j = 0; j < n; ++j) {
        PlaceEntry e;
        if (!entry(out[j].entry, e)) return 0;
        out[j].kind = e.kind;
        out[j].sub = e.sub;
        out[j].nums_n = e.nums_n;
        name(e.name_off, out[j].name, sizeof out[j].name);
        name(e.city_off, out[j].city, sizeof out[j].city);
    }
    return n;
}

bool SearchIndex::house(uint32_t i, uint32_t number, Point& out, bool& exact, uint8_t* fit) {
    PlaceEntry e;
    exact = false;
    if (fit) *fit = 3;
    if (!open_ || !entry(i, e)) return false;
    out = Point{e.lat_e7, e.lon_e7};
    if (e.nums_n == 0) return true;
    if (fit) *fit = 2;
    if (uint64_t(e.nums_off) + e.nums_n > h_.num_count) return false;
    auto at = [&](uint32_t k, HouseNumber& hn) {
        return cache_.read(h_.nums_off + (e.nums_off + k) * uint32_t(sizeof(HouseNumber)), &hn, sizeof hn);
    };
    uint32_t lo = 0, hi = e.nums_n;                 // first with number >= wanted
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        HouseNumber hn;
        if (!at(mid, hn)) return false;
        if (hn.number < number) lo = mid + 1;
        else hi = mid;
    }
    HouseNumber above{}, below{};
    const bool has_above = lo < e.nums_n && at(lo, above);
    const bool has_below = lo > 0 && at(lo - 1, below);
    if (has_above && above.number == number) {
        out = Point{above.lat_e7, above.lon_e7};
        exact = true;
        if (fit) *fit = 0;
    } else if (has_above && has_below) {
        if (fit) *fit = 1;
        // Between the two either side, in proportion: how a Garmin places
        // a number it has no point for.
        const double t = double(number - below.number) / double(above.number - below.number);
        out = Point{below.lat_e7 + int32_t((above.lat_e7 - below.lat_e7) * t),
                    below.lon_e7 + int32_t((above.lon_e7 - below.lon_e7) * t)};
    } else if (has_above) {
        out = Point{above.lat_e7, above.lon_e7};
    } else if (has_below) {
        out = Point{below.lat_e7, below.lon_e7};
    }
    return true;
}

}  // namespace core::route
