// core::route search ("Where to?") against an index the real builder made.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/route/search.hpp"
#include "search_build.hpp"

using namespace core::route;
using routegraph::RawPlace;

namespace {

class MemSource : public IBlockSource {
public:
    explicit MemSource(std::vector<uint8_t> b) : b_(std::move(b)) {}
    bool read(uint32_t off, void* dst, uint32_t len) override {
        if (off > b_.size() || len > b_.size() - off) return false;
        std::memcpy(dst, b_.data() + off, len);
        return true;
    }
    uint32_t size() const override { return uint32_t(b_.size()); }
    std::vector<uint8_t> b_;
};

Point pt(double lat, double lon) { return {int32_t(lat * 1e7), int32_t(lon * 1e7)}; }

RawPlace place(Place k, const char* name, const char* city, Point p, uint8_t sub = 0) {
    RawPlace r;
    r.kind = k;
    r.name = name;
    r.city = city;
    r.p = p;
    r.sub = sub;
    return r;
}

const Point kFay = pt(36.0626, -94.1574);       // Fayetteville square
const Point kSpr = pt(36.1867, -94.1288);       // Springdale

std::vector<RawPlace> town() {
    std::vector<RawPlace> v;
    RawPlace college = place(Place::Street, "N College Ave", "Fayetteville", pt(36.070, -94.1600));
    // 100 to 300 going north, a point every hundred.
    college.nums = {{300, pt(36.0720, -94.1600).lat_e7, pt(36.0720, -94.1600).lon_e7},
                    {100, pt(36.0700, -94.1600).lat_e7, pt(36.0700, -94.1600).lon_e7},
                    {200, pt(36.0710, -94.1600).lat_e7, pt(36.0710, -94.1600).lon_e7},
                    {200, pt(36.0999, -94.1600).lat_e7, pt(36.0999, -94.1600).lon_e7}};   // a duplicate
    v.push_back(college);
    v.push_back(place(Place::Street, "College St", "Springdale", kSpr));
    v.push_back(place(Place::Street, "W Dickson St", "Fayetteville", pt(36.0661, -94.1625)));
    v.push_back(place(Place::Town, "Fayetteville", "", kFay, 3));
    v.push_back(place(Place::Town, "Springdale", "", kSpr, 3));
    v.push_back(place(Place::Poi, "Dickson St Pub", "Fayetteville", pt(36.0662, -94.1610), PoiFood));
    v.push_back(place(Place::Poi, "Shell", "Springdale", pt(36.1800, -94.1300), PoiFuel));
    v.push_back(place(Place::Poi, "???", "", kFay));   // no searchable name: left out
    return v;
}

struct Rig {
    BlockCacheStorage<8> mem;
    BlockCache cache = mem.make();
    SearchIndex idx{cache};
    std::unique_ptr<MemSource> src;
};

}  // namespace

int main() {
    CASE("a name in search form");
    {
        char out[32];
        normalize("  N. College Ave.  ", out, sizeof out);
        CHECK(std::string(out) == "N COLLEGE AVE");
        normalize("Café 1st--St", out, sizeof out);
        CHECK(std::string(out) == "CAF 1ST ST");      // non-ASCII is a break, as on the keyboard
        normalize("Supercalifragilistic Expialidocious Road", out, 12);
        CHECK(std::strlen(out) == 11);
    }

    CASE("an entry is found by the start of any real word in it");
    {
        std::vector<std::string> keys;
        each_key("N COLLEGE AVE", [&](const char* k) { keys.push_back(k); });
        CHECK(keys.size() == 2 && keys[0] == "N COLLEGE AVE" && keys[1] == "COLLEGE AVE");
        keys.clear();
        each_key("US HIGHWAY 62", [&](const char* k) { keys.push_back(k); });
        CHECK(keys.size() == 2 && keys[1] == "62");
    }

    CASE("a house number typed first, then the street");
    {
        uint32_t num = 0;
        const char* street = nullptr;
        CHECK(split_house("13511 RYAN RD", num, street));
        CHECK(num == 13511 && std::string(street) == "RYAN RD");
        CHECK(split_house("12 ", num, street));
        CHECK(num == 12 && std::string(street).empty());
        CHECK(!split_house("13511", num, street));        // still typing the number
        CHECK(!split_house("62ND ST", num, street));      // a name that starts with digits
        CHECK(!split_house("1234567 X", num, street));    // too long for a house
        CHECK(!split_house("0 MAIN", num, street));
        CHECK(!split_house("MAIN", num, street));
    }

    const std::vector<uint8_t> file = routegraph::build_search(town(), 20261105);
    Rig rig;
    rig.src.reset(new MemSource(file));
    SearchIndex& s = rig.idx;
    CHECK(s.open(*rig.src));
    CHECK(s.header().entry_count == 7);                 // "???" dropped
    CHECK(s.header().osm_date == 20261105);

    CASE("the keyboard offers only letters that lead somewhere");
    {
        const auto has = [](uint64_t m, char c) {
            return (m >> (std::strchr(kAlphabet, c) - kAlphabet)) & 1;
        };
        const uint64_t first = s.next_chars("");
        CHECK(has(first, 'C') && has(first, 'D') && has(first, 'F') && has(first, 'S') && has(first, 'N'));
        CHECK(!has(first, 'Q') && !has(first, ' '));
        const uint64_t col = s.next_chars("COL");
        CHECK(has(col, 'L') && !has(col, 'A'));
        const uint64_t college = s.next_chars("COLLEGE");
        CHECK(has(college, ' '));
        const uint64_t college_ = s.next_chars("COLLEGE ");
        CHECK(has(college_, 'A') && has(college_, 'S') && !has(college_, 'B') && !has(college_, ' '));
        CHECK(s.next_chars("QX") == 0);
    }

    CASE("results nearest first, each place once, with its town");
    {
        SearchHit hits[8];
        uint32_t matches = 0;
        size_t n = s.find("COLLEGE", kFay, hits, 8, &matches);
        CHECK(n == 2);
        CHECK(matches == 2);
        CHECK(std::string(hits[0].name) == "N College Ave");
        CHECK(std::string(hits[0].city) == "Fayetteville");
        CHECK(hits[0].nums_n == 3);                     // the duplicate 200 dropped
        CHECK(std::string(hits[1].name) == "College St");
        CHECK(hits[0].dist_m < hits[1].dist_m);
        // From Springdale the order turns round.
        n = s.find("COLLEGE", kSpr, hits, 8);
        CHECK(std::string(hits[0].name) == "College St");
        // "DICKSON" is a street and a pub; "S" is a lot of things, capped.
        n = s.find("dickson", kFay, hits, 8);
        CHECK(n == 2);
        CHECK(hits[0].kind == Place::Poi || hits[1].kind == Place::Poi);
        n = s.find("S", kFay, hits, 2, &matches);
        CHECK(n == 2 && matches == 2);   // Shell, Springdale ("St" is filler, not a key)
        CHECK(s.find("NOWHERE", kFay, hits, 8) == 0);
    }

    CASE("a house number: exact, between two, or the end of the street");
    {
        SearchHit hit[1];
        s.find("N COLLEGE", kFay, hit, 1);
        Point p;
        bool exact = false;
        CHECK(s.house(hit[0].entry, 200, p, exact));
        CHECK(exact && p.lat_e7 == pt(36.0710, -94.16).lat_e7);
        CHECK(s.house(hit[0].entry, 250, p, exact));
        CHECK(!exact);
        CHECK_NEAR(p.lat_e7, pt(36.0715, -94.16).lat_e7, 2);
        CHECK(s.house(hit[0].entry, 50, p, exact));
        CHECK(!exact && p.lat_e7 == pt(36.0700, -94.16).lat_e7);
        CHECK(s.house(hit[0].entry, 9999, p, exact));
        CHECK(!exact && p.lat_e7 == pt(36.0720, -94.16).lat_e7);
        // A street with no numbers: its own point.
        s.find("DICKSON ST", kFay, hit, 1);
        CHECK(s.house(hit[0].entry, 12, p, exact));
        CHECK(!exact);
        CHECK(!s.house(999, 1, p, exact));
        // How well a number fits: exact, between, past the ends, none.
        uint8_t fit = 9;
        s.find("N COLLEGE", kFay, hit, 1);
        s.house(hit[0].entry, 200, p, exact, &fit);
        CHECK(fit == 0);
        s.house(hit[0].entry, 250, p, exact, &fit);
        CHECK(fit == 1);
        s.house(hit[0].entry, 9999, p, exact, &fit);
        CHECK(fit == 2);
        s.find("DICKSON ST", kFay, hit, 1);
        s.house(hit[0].entry, 12, p, exact, &fit);
        CHECK(fit == 3);
    }

    CASE("names as signed: spelled-out words shortened, and found by the short form");
    {
        std::vector<RawPlace> v;
        v.push_back(place(Place::Street, "North College Avenue", "Fayetteville", pt(36.07, -94.16)));
        v.push_back(place(Place::Town, "Little Rock", "", pt(34.7465, -92.2896), 3));
        v.push_back(place(Place::Street, "Little Rock Road", "Heber Springs", pt(35.49, -92.03)));
        const std::vector<uint8_t> f2 = routegraph::build_search(v, 20261105);
        Rig r2;
        r2.src.reset(new MemSource(f2));
        CHECK(r2.idx.open(*r2.src));
        SearchHit h[4];
        CHECK(r2.idx.find("N COLLEGE", kFay, h, 4) == 1);
        CHECK(std::string(h[0].name) == "N College Ave");
        CHECK(r2.idx.find("COLLEGE AVE", kFay, h, 4) == 1);
        // From Fayetteville the road is nearer, but "LITTLE ROCK" is the city.
        CHECK(r2.idx.find("LITTLE ROCK", kFay, h, 4) == 2);
        CHECK(h[0].kind == Place::Town && h[0].whole);
        CHECK(std::string(h[1].name) == "Little Rock Rd" && !h[1].whole);
        // A shop called just "Walmart" does not jump the queue: nearest wins.
        std::vector<RawPlace> w;
        w.push_back(place(Place::Poi, "Walmart", "Springdale", kSpr, PoiFood));
        w.push_back(place(Place::Poi, "Walmart Neighborhood Market", "Fayetteville", pt(36.065, -94.16)));
        const std::vector<uint8_t> f3 = routegraph::build_search(w, 20261105);
        Rig r3;
        r3.src.reset(new MemSource(f3));
        CHECK(r3.idx.open(*r3.src));
        CHECK(r3.idx.find("WALMART", kFay, h, 4) == 2);
        CHECK(std::string(h[0].city) == "Fayetteville");
        // "LITTLE" alone is the whole of neither: nearest first.
        CHECK(r2.idx.find("LITTLE", kFay, h, 4) == 2);
        CHECK(h[0].kind == Place::Street);
    }

    CASE("a foreign or cut file is refused");
    {
        std::vector<uint8_t> bad = file;
        bad[0] = 'X';
        MemSource b1(bad);
        CHECK(!s.open(b1));
        std::vector<uint8_t> cut(file.begin(), file.begin() + file.size() / 2);
        MemSource b2(cut);
        CHECK(!s.open(b2));
    }

    return check::report("route_search");
}
