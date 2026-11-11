#include "rgf_build.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <queue>

namespace routegraph {

using namespace core::route;

namespace {

constexpr uint32_t kInf = 0xFFFFFFFFu;

template <typename T>
bool read_vec(FILE* f, std::vector<T>& v, size_t n) {
    v.resize(n);
    return n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
}

// Packed exactly as extract.py's EDGE struct ("<IIIIIHBBB3x", 28 bytes).
#pragma pack(push, 1)
struct RawEdge {
    uint32_t u, v, length_dm, name_off, geom_off;
    uint16_t geom_n;
    uint8_t  cls, mph, flags, pad[3];
};
#pragma pack(pop)
static_assert(sizeof(RawEdge) == 28, "extract.py EDGE");

uint64_t hilbert(uint32_t x, uint32_t y) {   // 16-bit x, y
    uint64_t d = 0;
    for (uint32_t s = 1u << 15; s > 0; s >>= 1) {
        const uint32_t rx = (x & s) ? 1 : 0;
        const uint32_t ry = (y & s) ? 1 : 0;
        d += uint64_t(s) * s * ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = s - 1 - x; y = s - 1 - y; }
            std::swap(x, y);
        }
    }
    return d;
}

uint32_t weight_ds(const RgiEdge& e) {
    const double ds = e.length_dm / (route_mph(e) * 0.44704);
    return std::max<uint32_t>(1, uint32_t(ds + 0.5));
}

// --- contraction -------------------------------------------------------------------

struct Arc {
    uint32_t node, weight, aux;
};

struct UpRec {
    uint32_t target, weight, dir, aux;
};

class Contractor {
public:
    explicit Contractor(uint32_t n)
        : out_(n), in_(n), done_(n, 0), deleted_(n, 0), up_(n), dist_(n, kInf), stamp_(n, 0) {}

    void add_arc(uint32_t u, uint32_t v, uint32_t w, uint32_t aux) {
        for (Arc& a : out_[u])
            if (a.node == v) {
                if (w < a.weight) {
                    a.weight = w;
                    a.aux = aux;
                    for (Arc& b : in_[v])
                        if (b.node == u) { b.weight = w; b.aux = aux; }
                }
                return;
            }
        out_[u].push_back({v, w, aux});
        in_[v].push_back({u, w, aux});
    }

    uint32_t run() {
        const uint32_t n = uint32_t(out_.size());
        using Item = std::pair<int, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
        for (uint32_t v = 0; v < n; ++v) q.push({priority(v), v});
        uint32_t shortcuts = 0;
        while (!q.empty()) {
            const Item it = q.top();
            q.pop();
            if (done_[it.second]) continue;
            const int p = priority(it.second);
            if (!q.empty() && p > q.top().first) {
                q.push({p, it.second});
                continue;
            }
            shortcuts += contract(it.second);
        }
        return shortcuts;
    }

    const std::vector<UpRec>& up(uint32_t v) const { return up_[v]; }

private:
    static constexpr int kSettleLimit = 400;

    // Dijkstra from u, not through `skip`, as far as `limit`.
    void witness(uint32_t u, uint32_t skip, uint32_t limit) {
        ++cur_;
        using Item = std::pair<uint32_t, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
        set(u, 0);
        q.push({0, u});
        int settled = 0;
        while (!q.empty()) {
            const Item it = q.top();
            q.pop();
            if (it.first != get(it.second)) continue;
            if (it.first > limit || ++settled > kSettleLimit) break;
            for (const Arc& a : out_[it.second]) {
                if (a.node == skip) continue;
                const uint32_t d = it.first + a.weight;
                if (d < get(a.node)) {
                    set(a.node, d);
                    q.push({d, a.node});
                }
            }
        }
    }
    uint32_t get(uint32_t v) const { return stamp_[v] == cur_ ? dist_[v] : kInf; }
    void set(uint32_t v, uint32_t d) { stamp_[v] = cur_; dist_[v] = d; }

    // The shortcuts contracting v needs; added to `sc` when given.
    int shortcuts(uint32_t v, std::vector<std::pair<uint32_t, Arc>>* sc) {
        int count = 0;
        for (const Arc& in : in_[v]) {
            uint32_t limit = 0;
            for (const Arc& o : out_[v])
                if (o.node != in.node) limit = std::max(limit, in.weight + o.weight);
            if (limit == 0) continue;
            witness(in.node, v, limit);
            for (const Arc& o : out_[v]) {
                if (o.node == in.node) continue;
                const uint32_t w = in.weight + o.weight;
                if (get(o.node) > w) {
                    ++count;
                    if (sc) sc->push_back({in.node, {o.node, w, kAuxShortcut | v}});
                }
            }
        }
        return count;
    }

    int priority(uint32_t v) {
        const int removed = int(in_[v].size() + out_[v].size());
        return 2 * (shortcuts(v, nullptr) - removed) + int(deleted_[v]);
    }

    int contract(uint32_t v) {
        std::vector<std::pair<uint32_t, Arc>> sc;
        shortcuts(v, &sc);
        // Everything still attached to v goes to a higher node: these are
        // v's upward edges, stored at v.
        for (const Arc& o : out_[v]) up_[v].push_back({o.node, o.weight, kEdgeUp, o.aux});
        for (const Arc& i : in_[v]) up_[v].push_back({i.node, i.weight, kEdgeDown, i.aux});
        for (const Arc& i : in_[v]) {
            auto& l = out_[i.node];
            l.erase(std::remove_if(l.begin(), l.end(), [&](const Arc& a) { return a.node == v; }), l.end());
            ++deleted_[i.node];
        }
        for (const Arc& o : out_[v]) {
            auto& l = in_[o.node];
            l.erase(std::remove_if(l.begin(), l.end(), [&](const Arc& a) { return a.node == v; }), l.end());
            ++deleted_[o.node];
        }
        out_[v].clear();
        out_[v].shrink_to_fit();
        in_[v].clear();
        in_[v].shrink_to_fit();
        done_[v] = 1;
        for (const auto& s : sc) add_arc(s.first, s.second.node, s.second.weight, s.second.aux);
        return int(sc.size());
    }

    std::vector<std::vector<Arc>> out_, in_;
    std::vector<uint8_t> done_;
    std::vector<uint32_t> deleted_;
    std::vector<std::vector<UpRec>> up_;
    std::vector<uint32_t> dist_, stamp_;
    uint32_t cur_ = 0;
};

template <typename T>
void put(std::vector<uint8_t>& out, uint32_t off, const T& v) {
    std::memcpy(out.data() + off, &v, sizeof v);
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

// --- .rgi ------------------------------------------------------------------------

bool read_rgi(const std::string& path, Rgi& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    char magic[4];
    uint32_t h[6];
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "RGI1", 4) == 0 &&
              std::fread(h, 4, 6, f) == 6;
    std::vector<RawEdge> raw;
    std::vector<uint8_t> pois;
    ok = ok && read_vec(f, out.nodes, h[0]) && read_vec(f, raw, h[1]) && read_vec(f, out.geom, h[2]) &&
         read_vec(f, out.names, h[3]) && read_vec(f, pois, size_t(h[4]) * sizeof(Poi));
    std::fclose(f);
    if (!ok) { err = path + ": not an .rgi, or cut short"; return false; }
    out.osm_date = h[5];
    out.edges.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const RawEdge& r = raw[i];
        out.edges[i] = {r.u, r.v, r.length_dm, r.name_off, r.geom_off, r.geom_n, r.cls, r.mph, r.flags};
        if (r.u >= h[0] || r.v >= h[0] || size_t(r.geom_off) + r.geom_n > h[2] || r.name_off >= h[3]) {
            err = path + ": edge " + std::to_string(i) + " points outside the file";
            return false;
        }
    }
    out.pois.resize(h[4]);
    if (h[4]) std::memcpy(out.pois.data(), pois.data(), pois.size());
    return true;
}

int route_mph(const RgiEdge& e) {
    int mph = e.mph;
    if (mph == 0) {
        static const int kDefault[] = {25, 60, 55, 50, 45, 40, 25, 15, 15, 20, 70};
        mph = kDefault[e.cls < 11 ? e.cls : 0];
        if (e.flags & kAttrLink) mph = 35;
        if ((e.flags & kAttrDirt) && mph > 35) mph = 35;
    }
    return std::max(5, std::min(mph, kCruiseCapMph));
}

int posted_mph(const RgiEdge& e) {
    if (e.mph) return e.mph;
    if (e.cls == uint8_t(RoadClass::Motorway)) return 70;
    if (e.cls == uint8_t(RoadClass::Trunk)) return 65;
    if (e.cls == uint8_t(RoadClass::Primary)) return 55;
    return 0;
}

// --- .rgf ------------------------------------------------------------------------

std::vector<uint8_t> build_rgf(const Rgi& in, const Options& opt, Report* rep) {
    const uint32_t N = uint32_t(in.nodes.size());

    // Nodes along a Hilbert curve, so neighbours share card blocks.
    int32_t lat0 = INT32_MAX, lon0 = INT32_MAX, lat1 = INT32_MIN, lon1 = INT32_MIN;
    auto grow = [&](const Point& p) {
        lat0 = std::min(lat0, p.lat_e7); lat1 = std::max(lat1, p.lat_e7);
        lon0 = std::min(lon0, p.lon_e7); lon1 = std::max(lon1, p.lon_e7);
    };
    for (const Point& p : in.nodes) grow(p);
    for (const Point& p : in.geom) grow(p);
    if (N == 0) { lat0 = lon0 = lat1 = lon1 = 0; }
    auto key = [&](const Point& p) {
        const double sx = 65535.0 / std::max(1, lon1 - lon0), sy = 65535.0 / std::max(1, lat1 - lat0);
        return hilbert(uint32_t((p.lon_e7 - lon0) * sx), uint32_t((p.lat_e7 - lat0) * sy));
    };
    std::vector<uint32_t> order(N), remap(N);
    for (uint32_t i = 0; i < N; ++i) order[i] = i;
    std::vector<uint64_t> keys(N);
    for (uint32_t i = 0; i < N; ++i) keys[i] = key(in.nodes[i]);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
    for (uint32_t i = 0; i < N; ++i) remap[order[i]] = i;
    std::vector<Point> nodes(N);
    for (uint32_t i = 0; i < N; ++i) nodes[i] = in.nodes[order[i]];

    // Roads in the order of their first junction, geometry rewritten to match.
    const uint32_t E = uint32_t(in.edges.size());
    std::vector<uint32_t> eorder(E);
    for (uint32_t i = 0; i < E; ++i) eorder[i] = i;
    std::sort(eorder.begin(), eorder.end(), [&](uint32_t a, uint32_t b) {
        return remap[in.edges[a].u] < remap[in.edges[b].u];
    });
    std::vector<Attr> attrs(E);
    std::vector<Point> geom;
    geom.reserve(in.geom.size());
    for (uint32_t i = 0; i < E; ++i) {
        const RgiEdge& e = in.edges[eorder[i]];
        Attr& a = attrs[i];
        std::memset(&a, 0, sizeof a);
        a.from = remap[e.u];
        a.to = remap[e.v];
        a.geom_off = uint32_t(geom.size());
        a.geom_n = e.geom_n;
        a.name_off = e.name_off;
        a.length_dm = e.length_dm;
        a.weight_ds = weight_ds(e);
        a.cls = RoadClass(e.cls);
        a.mph = e.mph;
        a.flags = uint8_t(e.flags & (kAttrOneway | kAttrLink | kAttrDirt | kAttrRoundabout));
        a.cap_mph = uint8_t(std::min(posted_mph(e), 255));
        geom.insert(geom.end(), in.geom.begin() + e.geom_off, in.geom.begin() + e.geom_off + e.geom_n);
    }

    // The snapping grid.
    const uint32_t cell = std::max<uint32_t>(1, opt.cell_e7);
    const uint32_t gw = uint32_t((int64_t(lon1) - lon0) / cell + 1);
    const uint32_t gh = uint32_t((int64_t(lat1) - lat0) / cell + 1);
    std::vector<std::vector<uint32_t>> cells(size_t(gw) * gh);
    for (uint32_t i = 0; i < E; ++i) {
        const Attr& a = attrs[i];
        int32_t y0 = nodes[a.from].lat_e7, y1 = y0, x0 = nodes[a.from].lon_e7, x1 = x0;
        auto box = [&](const Point& p) {
            y0 = std::min(y0, p.lat_e7); y1 = std::max(y1, p.lat_e7);
            x0 = std::min(x0, p.lon_e7); x1 = std::max(x1, p.lon_e7);
        };
        box(nodes[a.to]);
        for (uint32_t k = 0; k < a.geom_n; ++k) box(geom[a.geom_off + k]);
        for (uint32_t cy = uint32_t((y0 - lat0) / cell); cy <= uint32_t((y1 - lat0) / cell); ++cy)
            for (uint32_t cx = uint32_t((x0 - lon0) / cell); cx <= uint32_t((x1 - lon0) / cell); ++cx)
                cells[size_t(cy) * gw + cx].push_back(i);
    }
    std::vector<uint32_t> grid(cells.size() + 1), items;
    for (size_t c = 0; c < cells.size(); ++c) {
        grid[c] = uint32_t(items.size());
        items.insert(items.end(), cells[c].begin(), cells[c].end());
    }
    grid[cells.size()] = uint32_t(items.size());

    std::vector<uint8_t> out(sizeof(Header), 0);
    Header h;
    std::memset(&h, 0, sizeof h);
    std::memcpy(h.magic, kMagic, 4);
    h.version = kVersion;
    h.osm_date = in.osm_date;
    h.node_count = N;
    h.nodes_off = append(out, nodes.data(), N);
    h.attrs_off = append(out, attrs.data(), E);
    h.attr_count = E;
    h.geom_off = append(out, geom.data(), geom.size());
    h.geom_count = uint32_t(geom.size());
    h.names_off = append(out, in.names.data(), in.names.size());
    h.names_bytes = uint32_t(in.names.size());
    h.pois_off = append(out, in.pois.data(), in.pois.size());
    h.poi_count = uint32_t(in.pois.size());
    h.grid_lat0_e7 = lat0;
    h.grid_lon0_e7 = lon0;
    h.cell_e7 = cell;
    h.grid_w = uint16_t(gw);
    h.grid_h = uint16_t(gh);
    h.grid_off = append(out, grid.data(), grid.size());
    h.cells_off = append(out, items.data(), items.size());
    h.cell_items = uint32_t(items.size());

    if (rep) { rep->nodes = N; rep->attrs = E; }
    const auto t0 = std::chrono::steady_clock::now();
    h.variant_count = uint16_t(std::min(opt.variants.size(), kMaxVariants));
    for (uint16_t vi = 0; vi < h.variant_count; ++vi) {
        const uint32_t vf = opt.variants[vi];
        Contractor c(N);
        for (uint32_t i = 0; i < E; ++i) {
            const Attr& a = attrs[i];
            if (!allowed(a, vf)) continue;
            c.add_arc(a.from, a.to, a.weight_ds, i);
            if (!(a.flags & kAttrOneway)) c.add_arc(a.to, a.from, a.weight_ds, i);
        }
        const uint32_t sc = c.run();
        std::vector<uint32_t> first(N + 1);
        std::vector<Edge> edges;
        for (uint32_t v = 0; v < N; ++v) {
            first[v] = uint32_t(edges.size());
            std::vector<UpRec> u = c.up(v);
            std::sort(u.begin(), u.end(), [](const UpRec& a, const UpRec& b) {
                return a.target != b.target ? a.target < b.target : a.weight < b.weight;
            });
            for (size_t k = 0; k < u.size(); ++k) {
                // Same target, weight and road both ways: one record.
                if (k + 1 < u.size() && u[k + 1].target == u[k].target && u[k + 1].weight == u[k].weight &&
                    u[k + 1].aux == u[k].aux) {
                    edges.push_back({u[k].target, u[k].weight | u[k].dir | u[k + 1].dir, u[k].aux});
                    ++k;
                } else {
                    edges.push_back({u[k].target, u[k].weight | u[k].dir, u[k].aux});
                }
            }
        }
        first[N] = uint32_t(edges.size());
        Variant& v = h.variants[vi];
        v.flags = vf;
        v.first_off = append(out, first.data(), first.size());
        v.edges_off = append(out, edges.data(), edges.size());
        v.edge_count = uint32_t(edges.size());
        if (rep) { rep->variant_edges.push_back(v.edge_count); rep->variant_shortcuts.push_back(sc); }
        if (opt.verbose)
            std::fprintf(stderr, "variant %u (flags %u): %u edges, %u shortcuts\n", vi, vf, v.edge_count, sc);
    }
    if (rep) rep->contract_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    put(out, 0, h);
    return out;
}

uint32_t reference_ds(const std::vector<uint8_t>& rgf, uint32_t vf, uint32_t from, uint32_t to) {
    Header h;
    std::memcpy(&h, rgf.data(), sizeof h);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> adj(h.node_count);
    for (uint32_t i = 0; i < h.attr_count; ++i) {
        Attr a;
        std::memcpy(&a, rgf.data() + h.attrs_off + i * sizeof(Attr), sizeof a);
        if (!allowed(a, vf)) continue;
        adj[a.from].push_back({a.to, a.weight_ds});
        if (!(a.flags & kAttrOneway)) adj[a.to].push_back({a.from, a.weight_ds});
    }
    std::vector<uint32_t> dist(h.node_count, kInf);
    using Item = std::pair<uint32_t, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    dist[from] = 0;
    q.push({0, from});
    while (!q.empty()) {
        const Item it = q.top();
        q.pop();
        if (it.first != dist[it.second]) continue;
        if (it.second == to) return it.first;
        for (const auto& e : adj[it.second])
            if (it.first + e.second < dist[e.first]) {
                dist[e.first] = it.first + e.second;
                q.push({dist[e.first], e.first});
            }
    }
    return kInf;
}

}  // namespace routegraph
