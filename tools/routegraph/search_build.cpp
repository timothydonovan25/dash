#include "search_build.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <map>

namespace routegraph {

using namespace core::route;

namespace {

// How a street sign spells it: "North College Avenue" is "N College Ave".
// Shorter on a 3.5" screen, and what a rider types. Whole words only.
std::string abbreviate(const std::string& name) {
    static const std::pair<const char*, const char*> kShort[] = {
        {"NORTH", "N"}, {"SOUTH", "S"}, {"EAST", "E"}, {"WEST", "W"},
        {"NORTHEAST", "NE"}, {"NORTHWEST", "NW"}, {"SOUTHEAST", "SE"}, {"SOUTHWEST", "SW"},
        {"STREET", "St"}, {"AVENUE", "Ave"}, {"ROAD", "Rd"}, {"DRIVE", "Dr"}, {"LANE", "Ln"},
        {"BOULEVARD", "Blvd"}, {"COURT", "Ct"}, {"PLACE", "Pl"}, {"HIGHWAY", "Hwy"},
        {"PARKWAY", "Pkwy"}, {"CIRCLE", "Cir"}, {"TRAIL", "Trl"}, {"TERRACE", "Ter"},
        {"COUNTY", "Co"}, {"MOUNT", "Mt"}, {"SAINT", "St"},
    };
    std::string out, word;
    auto flush = [&]() {
        if (word.empty()) return;
        std::string up = word;
        for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
        const char* rep = nullptr;
        for (const auto& kv : kShort)
            if (up == kv.first) rep = kv.second;
        out += rep ? rep : word;
        word.clear();
    };
    for (char c : name) {
        if (c == ' ') { flush(); out += ' '; }
        else word += c;
    }
    flush();
    return out;
}

template <typename T>
uint32_t append(std::vector<uint8_t>& out, const T* p, size_t n) {
    while (out.size() % 4) out.push_back(0);
    const uint32_t off = uint32_t(out.size());
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n * sizeof(T));
    return off;
}

}  // namespace

bool read_sxi(const std::string& path, std::vector<RawPlace>& out, uint32_t& osm_date, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    char magic[4];
    uint32_t h[4];
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SXI1", 4) == 0 && std::fread(h, 4, 4, f) == 4;
    std::vector<PlaceEntry> es(ok ? h[0] : 0);
    std::vector<HouseNumber> ns(ok ? h[1] : 0);
    std::vector<char> names(ok ? h[2] : 0);
    ok = ok && (es.empty() || std::fread(es.data(), sizeof(PlaceEntry), es.size(), f) == es.size()) &&
         (ns.empty() || std::fread(ns.data(), sizeof(HouseNumber), ns.size(), f) == ns.size()) &&
         (names.empty() || std::fread(names.data(), 1, names.size(), f) == names.size());
    std::fclose(f);
    if (!ok) { err = path + ": not an .sxi, or cut short"; return false; }
    osm_date = h[3];
    auto str = [&](uint32_t off) { return off < names.size() ? std::string(names.data() + off) : std::string(); };
    out.clear();
    out.reserve(es.size());
    for (const PlaceEntry& e : es) {
        if (uint64_t(e.nums_off) + e.nums_n > ns.size()) { err = path + ": numbers out of range"; return false; }
        RawPlace r;
        r.kind = e.kind;
        r.sub = e.sub;
        r.name = str(e.name_off);
        r.city = str(e.city_off);
        r.p = Point{e.lat_e7, e.lon_e7};
        r.nums.assign(ns.begin() + e.nums_off, ns.begin() + e.nums_off + e.nums_n);
        out.push_back(std::move(r));
    }
    return true;
}

std::vector<uint8_t> build_search(const std::vector<RawPlace>& places, uint32_t osm_date) {
    std::vector<char> names(1, '\0');
    std::map<std::string, uint32_t> name_at{{"", 0}};
    auto name = [&](const std::string& s) {
        auto it = name_at.find(s);
        if (it != name_at.end()) return it->second;
        const uint32_t off = uint32_t(names.size());
        names.insert(names.end(), s.begin(), s.end());
        names.push_back('\0');
        name_at.emplace(s, off);
        return off;
    };
    std::vector<PlaceEntry> entries;
    std::vector<HouseNumber> nums;
    std::vector<SearchKey> keys;
    for (const RawPlace& r0 : places) {
        RawPlace r = r0;
        r.name = abbreviate(r.name);
        char norm[64];
        if (normalize(r.name.c_str(), norm, sizeof norm) == 0) continue;
        PlaceEntry e{};
        e.kind = r.kind;
        e.sub = r.sub;
        e.name_off = name(r.name);
        e.city_off = name(r.city);
        e.lat_e7 = r.p.lat_e7;
        e.lon_e7 = r.p.lon_e7;
        // House numbers by number, one point each.
        std::vector<HouseNumber> n = r.nums;
        std::stable_sort(n.begin(), n.end(), [](const HouseNumber& a, const HouseNumber& b) { return a.number < b.number; });
        n.erase(std::unique(n.begin(), n.end(), [](const HouseNumber& a, const HouseNumber& b) { return a.number == b.number; }),
                n.end());
        e.nums_off = uint32_t(nums.size());
        e.nums_n = uint32_t(n.size());
        nums.insert(nums.end(), n.begin(), n.end());
        const uint32_t idx = uint32_t(entries.size());
        entries.push_back(e);
        bool full = true;    // each_key gives the whole name first
        each_key(norm, [&](const char* k) {
            SearchKey sk{};
            const size_t n = std::strlen(k);
            std::memcpy(sk.key, k, n < kKeyLen ? n : kKeyLen);   // cut to 24; the rest stays NUL
            sk.entry = idx | (full ? kFullKey : 0) | (r.kind == Place::Town ? kTownKey : 0);
            full = false;
            sk.lat_e7 = r.p.lat_e7;
            sk.lon_e7 = r.p.lon_e7;
            keys.push_back(sk);
        });
    }
    std::sort(keys.begin(), keys.end(), [](const SearchKey& a, const SearchKey& b) {
        const int c = std::memcmp(a.key, b.key, kKeyLen);
        const uint32_t ma = a.entry & kEntryMask, mb = b.entry & kEntryMask;
        if (c != 0) return c < 0;
        return ma != mb ? ma < mb : (a.entry & kFullKey) > (b.entry & kFullKey);   // full key first
    });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const SearchKey& a, const SearchKey& b) {
                   // "LITTLE ROCK" twice for one entry: keep the full-name one.
                   return (a.entry & kEntryMask) == (b.entry & kEntryMask) &&
                          std::memcmp(a.key, b.key, kKeyLen) == 0;
               }),
               keys.end());

    std::vector<uint8_t> out(sizeof(SearchHeader), 0);
    SearchHeader h{};
    std::memcpy(h.magic, kSearchMagic, 4);
    h.version = kSearchVersion;
    h.osm_date = osm_date;
    h.entry_count = uint32_t(entries.size());
    h.entries_off = append(out, entries.data(), entries.size());
    h.num_count = uint32_t(nums.size());
    h.nums_off = append(out, nums.data(), nums.size());
    h.key_count = uint32_t(keys.size());
    h.keys_off = append(out, keys.data(), keys.size());
    h.names_off = append(out, names.data(), names.size());
    h.names_bytes = uint32_t(names.size());
    std::memcpy(out.data(), &h, sizeof h);
    return out;
}

}  // namespace routegraph
