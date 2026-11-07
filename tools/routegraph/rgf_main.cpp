// rgf_build: .rgi -> .rgf, and a self-check of the result.
//
//   rgf_build in.rgi out.rgf [--check N]
//   rgf_build --check N --rgf map.rgf
//   rgf_build --rgf map.rgf --route LAT,LON LAT,LON [--flags F]
//   rgf_build --search in.sxi search.idx
//   rgf_build --idx search.idx --find TEXT LAT,LON [NUMBER]
//
// --search writes the "Where to?" index (core/route/search.hpp) from what
// extract.py found; --find searches it the way the dash does, nearest
// first, and with a NUMBER places that house on the first street found.
//
// --route prints one route the way the dash would ride it: distance, time,
// 65 mph roads, and each cue with its road. F is the variant: 1 no 65 mph
// roads, 2 no dirt, 3 neither.
//
// --check runs N random node-to-node routes per variant through the
// dash's own query code (core::route) and holds each against plain
// Dijkstra on the uncontracted roads; it also counts card-block reads with
// a cache the size the dash will have, which is the number that says
// whether the SD card is fast enough.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "core/route/route.hpp"
#include "rgf_build.hpp"
#include "search_build.hpp"

using namespace core::route;

namespace {

class MemSource : public IBlockSource {
public:
    explicit MemSource(const std::vector<uint8_t>& b) : b_(b) {}
    bool read(uint32_t off, void* dst, uint32_t len) override {
        if (off > b_.size() || len > b_.size() - off) return false;
        std::memcpy(dst, b_.data() + off, len);
        ++reads_;
        return true;
    }
    uint32_t size() const override { return uint32_t(b_.size()); }
    uint32_t reads_ = 0;
private:
    const std::vector<uint8_t>& b_;
};

struct Big {
    BlockCacheStorage<1024> cache;        // 4 MB, what the dash gives it
    SearchStorage<65536> search;
    RouteStorage<32768, 1024> route;
};

int check(const std::vector<uint8_t>& rgf, int n, uint32_t seed) {
    auto big = std::make_unique<Big>();
    BlockCache cache = big->cache.make();
    Graph g(cache);
    MemSource src(rgf);
    const OpenResult o = g.open(src);
    if (o != OpenResult::Ok) { std::fprintf(stderr, "open: %s\n", to_string(o)); return 1; }
    const Header& h = g.header();
    // A road leaving each node, to stand the ends of a route on.
    std::vector<uint32_t> leaving(h.node_count, kNone);
    for (uint32_t i = 0; i < h.attr_count; ++i) {
        Attr a;
        g.attr(i, a);
        if (leaving[a.from] == kNone) leaving[a.from] = i;
    }
    std::mt19937 rng(seed);
    int bad = 0;
    for (int v = 0; v < h.variant_count; ++v) {
        const uint32_t vf = h.variants[v].flags;
        int done = 0, unreachable = 0;
        uint64_t settled = 0, misses = 0, max_settled = 0, max_misses = 0, pts = 0;
        double ms = 0;
        for (int tries = 0; done < n && tries < n * 20; ++tries) {
            const uint32_t s = rng() % h.node_count, t = rng() % h.node_count;
            if (leaving[s] == kNone || leaving[t] == kNone) continue;
            Attr as, at;
            g.attr(leaving[s], as);
            g.attr(leaving[t], at);
            if (!allowed(as.flags, vf) || !allowed(at.flags, vf)) continue;
            const uint32_t want = routegraph::reference_ds(rgf, vf, s, t);
            Snap a, b;
            a.attr = leaving[s]; a.along_dm = 0; a.length_dm = 1;
            b.attr = leaving[t]; b.along_dm = 0; b.length_dm = 1;
            Route r = big->route.make();
            QueryStats st;
            cache.attach(&src);             // a cold cache: the worst case
            const auto t0 = std::chrono::steady_clock::now();
            const Status q = query(g, vf, a, b, big->search.mem(), r, &st);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (want == 0xFFFFFFFFu) {
                ++unreachable;
                if (q != Status::NoRoute) { ++bad; std::fprintf(stderr, "v%d %u->%u: unreachable but %s\n", v, s, t, to_string(q)); }
                continue;
            }
            if (q != Status::Ok || st.weight_ds != want) {
                ++bad;
                std::fprintf(stderr, "v%d %u->%u: %s, %u ds, want %u\n", v, s, t, to_string(q), st.weight_ds, want);
                continue;
            }
            ++done;
            settled += st.settled;
            misses += st.cache_misses;
            pts += r.n_pts;
            if (st.settled > max_settled) max_settled = st.settled;
            if (st.cache_misses > max_misses) max_misses = st.cache_misses;
        }
        std::printf("variant %d (flags %u): %d routes match Dijkstra, %d unreachable; settled avg %llu max %llu; "
                    "block reads avg %llu max %llu (cold 4 MB cache); %llu points avg; %.1f ms avg on this PC\n",
                    v, vf, done, unreachable, (unsigned long long)(done ? settled / done : 0),
                    (unsigned long long)max_settled, (unsigned long long)(done ? misses / done : 0),
                    (unsigned long long)max_misses, (unsigned long long)(done ? pts / done : 0),
                    done + unreachable ? ms / (done + unreachable) : 0.0);
    }
    std::printf("%s: %d mismatches\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}

int route(const std::vector<uint8_t>& rgf, const char* a, const char* b, uint32_t vf) {
    auto big = std::make_unique<Big>();
    BlockCache cache = big->cache.make();
    Graph g(cache);
    MemSource src(rgf);
    if (g.open(src) != OpenResult::Ok) { std::fprintf(stderr, "not a map\n"); return 1; }
    double la, lo, lb, ob;
    if (std::sscanf(a, "%lf,%lf", &la, &lo) != 2 || std::sscanf(b, "%lf,%lf", &lb, &ob) != 2) {
        std::fprintf(stderr, "--route wants LAT,LON LAT,LON\n");
        return 2;
    }
    const auto pt = [](double lat, double lon) {
        return Point{int32_t(lat * 1e7 + (lat < 0 ? -0.5 : 0.5)), int32_t(lon * 1e7 + (lon < 0 ? -0.5 : 0.5))};
    };
    Route r = big->route.make();
    QueryStats st;
    const Status q = query(g, vf, pt(la, lo), pt(lb, ob), big->search.mem(), r, &st);
    std::printf("%s: %.2f mi, %.0f min, %u points; 65 mph roads %.2f mi; settled %u, block reads %u\n",
                to_string(q), r.total_dm / 16093.44, r.total_ds / 600.0, r.n_pts, r.fast_dm / 16093.44,
                st.settled, st.cache_misses);
    for (uint32_t i = 0; i < r.n_cues; ++i) {
        char name[64];
        g.name(r.cues[i].name_off, name, sizeof name);
        std::printf("  %6.2f mi  %-12s %4d deg  %s%s\n", r.cues[i].dist_dm / 16093.44,
                    to_string(r.cues[i].kind), r.cues[i].turn_deg, name,
                    (r.cues[i].flags & kAttrFast) ? "  [65]" : "");
    }
    return q == Status::Ok ? 0 : 1;
}

bool load(const char* path, std::vector<uint8_t>& out);

int find(const char* idx_path, const char* text, const char* at, const char* number) {
    std::vector<uint8_t> bytes;
    if (!load(idx_path, bytes)) { std::fprintf(stderr, "cannot read %s\n", idx_path); return 1; }
    auto mem = std::make_unique<BlockCacheStorage<256>>();
    BlockCache cache = mem->make();
    SearchIndex idx(cache);
    MemSource src(bytes);
    if (!idx.open(src)) { std::fprintf(stderr, "not a search index\n"); return 1; }
    double lat, lon;
    if (std::sscanf(at, "%lf,%lf", &lat, &lon) != 2) { std::fprintf(stderr, "LAT,LON\n"); return 2; }
    const Point near{int32_t(lat * 1e7), int32_t(lon * 1e7)};
    char norm[32];
    normalize(text, norm, sizeof norm);
    uint32_t matches = 0;
    SearchHit hits[10];
    cache.reset_counts();
    const size_t n = idx.find(norm, near, hits, 10, &matches);
    const uint64_t next = idx.next_chars(norm);
    char letters[40];
    size_t k = 0;
    for (size_t i = 0; i < kAlphabetLen; ++i)
        if (next >> i & 1) letters[k++] = kAlphabet[i] == ' ' ? '_' : kAlphabet[i];
    letters[k] = '\0';
    std::printf("\"%s\": %u keys match; next letters %s; %u block reads\n", norm, matches, letters,
                cache.misses());
    static const char* const kKind[] = {"?", "street", "town", "place"};
    for (size_t i = 0; i < n; ++i)
        std::printf("  %6.1f mi  %-6s %-24s %-16s %u numbers\n", hits[i].dist_m / 1609.344,
                    kKind[int(hits[i].kind) & 3], hits[i].name, hits[i].city, hits[i].nums_n);
    static const char* const kFit[] = {"exact", "between two", "past the ends", "no numbers"};
    for (size_t i = 0; number && i < n; ++i) {
        if (hits[i].kind != Place::Street) continue;
        Point p;
        bool exact = false;
        uint8_t fit = 3;
        idx.house(hits[i].entry, uint32_t(std::atoi(number)), p, exact, &fit);
        std::printf("  %s %s, %s: %.6f,%.6f (%s)\n", number, hits[i].name, hits[i].city, p.lat_e7 * 1e-7,
                    p.lon_e7 * 1e-7, kFit[fit]);
    }
    return n ? 0 : 1;
}

bool load(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    out.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const char* in = nullptr;
    const char* outp = nullptr;
    const char* rgf_path = nullptr;
    int n_check = 0;
    const char* route_a = nullptr;
    const char* route_b = nullptr;
    uint32_t flags = 0;
    if (argc >= 4 && !std::strcmp(argv[1], "--search")) {
        std::vector<routegraph::RawPlace> places;
        uint32_t date = 0;
        std::string err;
        if (!routegraph::read_sxi(argv[2], places, date, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        const std::vector<uint8_t> idx = routegraph::build_search(places, date);
        FILE* f = std::fopen(argv[3], "wb");
        if (!f || std::fwrite(idx.data(), 1, idx.size(), f) != idx.size()) { std::fprintf(stderr, "cannot write\n"); return 1; }
        std::fclose(f);
        SearchHeader h;
        std::memcpy(&h, idx.data(), sizeof h);
        std::printf("%s: %.1f MB, %u places, %u house numbers, %u keys\n", argv[3], idx.size() / 1e6,
                    h.entry_count, h.num_count, h.key_count);
        return 0;
    }
    if (argc >= 6 && !std::strcmp(argv[1], "--idx") && !std::strcmp(argv[3], "--find"))
        return find(argv[2], argv[4], argv[5], argc > 6 ? argv[6] : nullptr);
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check") && i + 1 < argc) n_check = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--route") && i + 2 < argc) { route_a = argv[++i]; route_b = argv[++i]; }
        else if (!std::strcmp(argv[i], "--flags") && i + 1 < argc) flags = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--rgf") && i + 1 < argc) rgf_path = argv[++i];
        else if (!in) in = argv[i];
        else if (!outp) outp = argv[i];
    }
    std::vector<uint8_t> rgf;
    if (rgf_path) {
        if (!load(rgf_path, rgf)) { std::fprintf(stderr, "cannot read %s\n", rgf_path); return 1; }
    } else {
        if (!in || !outp) {
            std::fprintf(stderr, "usage: rgf_build in.rgi out.rgf [--check N]\n"
                                 "       rgf_build --check N --rgf map.rgf\n");
            return 2;
        }
        routegraph::Rgi g;
        std::string err;
        if (!routegraph::read_rgi(in, g, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        routegraph::Options opt;
        opt.verbose = true;
        routegraph::Report rep;
        rgf = routegraph::build_rgf(g, opt, &rep);
        FILE* f = std::fopen(outp, "wb");
        if (!f || std::fwrite(rgf.data(), 1, rgf.size(), f) != rgf.size()) {
            std::fprintf(stderr, "cannot write %s\n", outp);
            return 1;
        }
        std::fclose(f);
        std::printf("%s: %.1f MB, %u nodes, %u roads, contraction %.0f s\n", outp, rgf.size() / 1e6,
                    rep.nodes, rep.attrs, rep.contract_s);
    }
    if (route_a) return route(rgf, route_a, route_b, flags);
    return n_check ? check(rgf, n_check, 1) : 0;
}
