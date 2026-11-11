#include "core/route/route.hpp"
#include <cmath>
#include <cstring>
#include <utility>

namespace core::route {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthM = 6371000.0;
constexpr uint32_t kInf = 0xFFFFFFFFu;

// --- one direction of the search ---------------------------------------------

struct Side {
    SearchSide m;
    uint32_t heap_n = 0;
    uint32_t used = 0;
    bool overflow = false;

    void clear() {
        for (uint32_t i = 0; i < m.cap; ++i) m.table[i].node = kNone;
        heap_n = used = 0;
        overflow = false;
    }
    SearchEntry* find(uint32_t node) {
        for (uint32_t i = (node * 2654435761u) & (m.cap - 1);; i = (i + 1) & (m.cap - 1)) {
            if (m.table[i].node == node) return &m.table[i];
            if (m.table[i].node == kNone) return nullptr;
        }
    }
    SearchEntry* insert(uint32_t node) {
        if (used * 4 >= m.cap * 3) { overflow = true; return nullptr; }   // keep probes short
        uint32_t i = (node * 2654435761u) & (m.cap - 1);
        while (m.table[i].node != kNone) i = (i + 1) & (m.cap - 1);
        ++used;
        SearchEntry& e = m.table[i];
        e = {node, kInf, kNone, 0, 0};
        return &e;
    }
    void push(uint32_t dist, uint32_t node) {
        if (heap_n == m.cap) { overflow = true; return; }
        uint32_t i = heap_n++;
        while (i > 0) {
            const uint32_t p = (i - 1) / 2;
            if (m.heap[p].dist <= dist) break;
            m.heap[i] = m.heap[p];
            i = p;
        }
        m.heap[i] = {dist, node};
    }
    HeapItem pop() {
        const HeapItem top = m.heap[0];
        const HeapItem last = m.heap[--heap_n];
        uint32_t i = 0;
        for (;;) {
            uint32_t c = 2 * i + 1;
            if (c >= heap_n) break;
            if (c + 1 < heap_n && m.heap[c + 1].dist < m.heap[c].dist) ++c;
            if (m.heap[c].dist >= last.dist) break;
            m.heap[i] = m.heap[c];
            i = c;
        }
        if (heap_n) m.heap[i] = last;
        return top;
    }
    // Drop stale heap entries; the smallest live distance, or kInf.
    uint32_t top() {
        while (heap_n) {
            const HeapItem& h = m.heap[0];
            const SearchEntry* e = find(h.node);
            if (e && !e->settled && e->dist == h.dist) return h.dist;
            pop();
        }
        return kInf;
    }
    // Reach `node` at `dist`; false only on overflow.
    bool reach(uint32_t node, uint32_t dist, uint32_t parent, uint32_t aux) {
        SearchEntry* e = find(node);
        if (!e) e = insert(node);
        if (!e) return false;
        if (e->settled || dist >= e->dist) return true;
        e->dist = dist;
        e->parent = parent;
        e->aux = aux;
        push(dist, node);
        return !overflow;
    }
};

uint32_t part(uint32_t weight, uint32_t num, uint32_t den) {
    if (den == 0) return 0;
    return static_cast<uint32_t>(uint64_t(weight) * num / den);
}

// --- laying the path out as points -------------------------------------------

// One road on the path, in travel order: `reversed` runs it to -> from;
// [start_dm, end_dm) is measured from the end it is entered by.
struct Leg {
    uint32_t attr;
    bool     reversed;
    uint32_t start_dm;
    uint32_t end_dm;     // kInf: to the far end
};

class Builder {
public:
    Builder(Graph& g, Route& r) : g_(g), r_(r) {}
    bool ok() const { return ok_; }
    Status status() const { return status_; }

    void add(const Leg& leg);
    void finish();

private:
    bool point_k(const Attr& a, uint32_t k, bool reversed, Point& out);
    void emit(Point p, uint32_t dist_dm, uint32_t time_ds, const Attr& a);
    void fail(Status s) { if (ok_) { ok_ = false; status_ = s; } }
    bool same_street(uint32_t a, uint32_t b);

    Graph& g_;
    Route& r_;
    bool ok_ = true;
    Status status_ = Status::Ok;
    uint32_t dist_dm_ = 0;
    uint32_t time_ds_ = 0;
    bool first_ = true;
};

bool Builder::point_k(const Attr& a, uint32_t k, bool reversed, Point& out) {
    const uint32_t n = a.geom_n + 2u;
    const uint32_t i = reversed ? n - 1 - k : k;
    if (i == 0) return g_.node(a.from, out);
    if (i == n - 1) return g_.node(a.to, out);
    return g_.geom(a.geom_off + i - 1, out);
}

void Builder::emit(Point p, uint32_t dist_dm, uint32_t time_ds, const Attr& a) {
    if (r_.n_pts == r_.pts_cap) { fail(Status::TooLong); return; }
    RoutePoint& q = r_.pts[r_.n_pts++];
    q.p = p;
    q.dist_dm = dist_dm;
    q.time_ds = time_ds;
    q.name_off = a.name_off;
    q.mph = a.mph;
    q.flags = a.flags;
    q.pad[0] = q.pad[1] = 0;
}

void Builder::add(const Leg& leg) {
    if (!ok_) return;
    Attr a;
    if (!g_.attr(leg.attr, a)) { fail(Status::ReadError); return; }
    const uint32_t n = a.geom_n + 2u;
    // Pass 1: the road's length as this code measures it, so the clip
    // points agree with snap(), which measures the same way.
    double total = 0;
    Point prev{}, p{};
    for (uint32_t k = 0; k < n; ++k) {
        if (!point_k(a, k, leg.reversed, p)) { fail(Status::ReadError); return; }
        if (k) total += dist_m(prev, p) * 10.0;
        prev = p;
    }
    const double start = leg.start_dm;
    const double end = leg.end_dm == kInf ? total : (leg.end_dm < total ? leg.end_dm : total);
    if (end <= start && !first_) return;   // nothing of this road is travelled
    const double ds_per_dm = total > 0 ? a.weight_ds / total : 0;

    // A new road starts at the junction the last one ended on: mark it for
    // a cue, unless this is the first road.
    if (!first_ && r_.n_cues < r_.cues_cap && r_.n_pts) {
        Cue& c = r_.cues[r_.n_cues++];
        c.point = r_.n_pts - 1;
        c.dist_dm = dist_dm_;
        c.name_off = a.name_off;
        c.turn_deg = 0;
        c.kind = CueKind::Straight;
        c.flags = a.flags;
        // The junction point is where the new road's segment starts.
        r_.pts[r_.n_pts - 1].name_off = a.name_off;
        r_.pts[r_.n_pts - 1].mph = a.mph;
        r_.pts[r_.n_pts - 1].flags = a.flags;
    }

    const uint32_t base_dm = dist_dm_;
    const uint32_t base_ds = time_ds_;
    auto at = [&](double along) {
        return std::pair<uint32_t, uint32_t>(base_dm + uint32_t(along - start + 0.5),
                                             base_ds + uint32_t((along - start) * ds_per_dm + 0.5));
    };
    double c0 = 0;
    for (uint32_t k = 0; k < n && ok_; ++k) {
        if (!point_k(a, k, leg.reversed, p)) { fail(Status::ReadError); return; }
        const double c1 = k ? c0 + dist_m(prev, p) * 10.0 : 0.0;
        if (k == 0 ? start <= 0 : (c0 <= start && start < c1) ) {
            // The start point: interpolated on this segment (or the first point).
            Point s = p;
            if (k && c1 > c0) {
                const double t = (start - c0) / (c1 - c0);
                s.lat_e7 = prev.lat_e7 + int32_t(std::lround((p.lat_e7 - prev.lat_e7) * t));
                s.lon_e7 = prev.lon_e7 + int32_t(std::lround((p.lon_e7 - prev.lon_e7) * t));
            }
            if (first_ || !r_.n_pts) {
                const auto d = at(start);
                emit(s, d.first, d.second, a);
            }
        }
        if (k && c1 > start && c1 < end) {
            const auto d = at(c1);
            emit(p, d.first, d.second, a);
        }
        if (k && c0 < end && end <= c1) {
            Point e = p;
            if (c1 > c0) {
                const double t = (end - c0) / (c1 - c0);
                e.lat_e7 = prev.lat_e7 + int32_t(std::lround((p.lat_e7 - prev.lat_e7) * t));
                e.lon_e7 = prev.lon_e7 + int32_t(std::lround((p.lon_e7 - prev.lon_e7) * t));
            }
            const auto d = at(end);
            if (!(r_.n_pts && r_.pts[r_.n_pts - 1].dist_dm == d.first &&
                  r_.pts[r_.n_pts - 1].p.lat_e7 == e.lat_e7 && r_.pts[r_.n_pts - 1].p.lon_e7 == e.lon_e7))
                emit(e, d.first, d.second, a);
            break;
        }
        c0 = c1;
        prev = p;
    }
    if (!ok_) return;
    const uint32_t len = end > start ? uint32_t(end - start + 0.5) : 0;
    if (a.flags & kAttrFast) {
        if (r_.fast_dm == 0) r_.first_fast_name = a.name_off;
        r_.fast_dm += len;
    }
    dist_dm_ = base_dm + len;
    time_ds_ = base_ds + uint32_t((end - start) * ds_per_dm + 0.5);
    first_ = false;
}

// A point about `back_dm` behind (or ahead of) index i along the polyline.
uint32_t walk(const Route& r, uint32_t i, int dir, uint32_t span_dm) {
    const uint32_t d0 = r.pts[i].dist_dm;
    uint32_t j = i;
    while (dir < 0 ? j > 0 : j + 1 < r.n_pts) {
        j = dir < 0 ? j - 1 : j + 1;
        const uint32_t d = r.pts[j].dist_dm;
        if ((dir < 0 ? d0 - d : d - d0) >= span_dm) break;
    }
    return j;
}

constexpr uint32_t kCueSpanDm = 200;     // bearings measured over 20 m
constexpr int kTurnDeg = 30;             // below this, a turn is "straight on"

CueKind kind_of(double t) {
    const double a = t < 0 ? -t : t;
    if (a < kTurnDeg) return CueKind::Straight;
    if (a >= 170) return CueKind::UTurn;
    const bool right = t > 0;
    if (a < 60)  return right ? CueKind::SlightRight : CueKind::SlightLeft;
    if (a < 135) return right ? CueKind::Right : CueKind::Left;
    return right ? CueKind::SharpRight : CueKind::SharpLeft;
}

// East North Street becoming West North Street is the same street to a
// rider: compare names with a leading compass word dropped.
bool Builder::same_street(uint32_t a, uint32_t b) {
    char na[48], nb[48];
    g_.name(a, na, sizeof na);
    g_.name(b, nb, sizeof nb);
    const auto bare = [](const char* n) {
        static const char* const kWords[] = {"North ", "South ", "East ", "West ", "N ", "S ", "E ", "W "};
        for (const char* w : kWords) {
            const size_t k = std::strlen(w);
            if (std::strncmp(n, w, k) == 0) return n + k;
        }
        return n;
    };
    return na[0] && std::strcmp(bare(na), bare(nb)) == 0;
}

void Builder::finish() {
    if (!ok_) return;
    r_.total_dm = dist_dm_;
    r_.total_ds = time_ds_;
    // Keep a cue where the road turns or changes name; drop the rest (the
    // junctions where the route simply carries on).
    uint32_t kept = 0;
    for (uint32_t c = 0; c < r_.n_cues; ++c) {
        Cue cue = r_.cues[c];
        const uint32_t i = cue.point;
        if (i == 0 || i + 1 >= r_.n_pts) continue;
        // A roundabout is one cue, at the way in: the junctions on the ring
        // are not turns the rider makes. Its angle is from the way in to
        // the way out, and its road is the one it leaves by.
        if ((cue.flags & kAttrRoundabout) && !(r_.pts[i - 1].flags & kAttrRoundabout)) {
            uint32_t k = c + 1;
            while (k < r_.n_cues && (r_.cues[k].flags & kAttrRoundabout)) ++k;
            const uint32_t b = walk(r_, i, -1, kCueSpanDm);
            double t = 0;
            if (k < r_.n_cues) {
                const uint32_t x = r_.cues[k].point;
                const uint32_t f = walk(r_, x, +1, kCueSpanDm);
                t = turn_deg(bearing_deg(r_.pts[b].p, r_.pts[i].p), bearing_deg(r_.pts[x].p, r_.pts[f].p));
                cue.name_off = r_.cues[k].name_off;
            }
            cue.kind = CueKind::Roundabout;
            cue.turn_deg = int16_t(std::lround(t));
            if (kept && r_.cues[kept - 1].dist_dm + 1 >= cue.dist_dm) --kept;
            r_.cues[kept++] = cue;
            c = k;                       // the way out is part of this cue
            continue;
        }
        if (cue.flags & kAttrRoundabout) continue;   // on the ring, started on it
        const uint32_t b = walk(r_, i, -1, kCueSpanDm);
        const uint32_t f = walk(r_, i, +1, kCueSpanDm);
        const double t = turn_deg(bearing_deg(r_.pts[b].p, r_.pts[i].p),
                                  bearing_deg(r_.pts[i].p, r_.pts[f].p));
        const uint32_t before = r_.pts[i - 1].name_off;
        const bool renamed = cue.name_off != 0 && cue.name_off != before && !same_street(before, cue.name_off);
        cue.turn_deg = int16_t(std::lround(t));
        cue.kind = kind_of(t);
        if (cue.kind == CueKind::Straight && !renamed) continue;
        // Two cues on one junction (a zero-length road between them): the later wins.
        if (kept && r_.cues[kept - 1].dist_dm + 1 >= cue.dist_dm) --kept;
        r_.cues[kept++] = cue;
    }
    r_.n_cues = kept;
    if (r_.n_pts && r_.n_cues < r_.cues_cap) {
        Cue& c = r_.cues[r_.n_cues++];
        c.point = r_.n_pts - 1;
        c.dist_dm = r_.total_dm;
        c.name_off = r_.pts[r_.n_pts - 1].name_off;
        c.turn_deg = 0;
        c.kind = CueKind::Arrive;
        c.flags = 0;
    } else if (r_.n_pts) {
        fail(Status::TooLong);
    }
}

}  // namespace

// --- geometry ------------------------------------------------------------------

double dist_m(Point a, Point b) {
    const double la = (a.lat_e7 + b.lat_e7) * 0.5e-7 * kPi / 180.0;
    const double dy = (b.lat_e7 - a.lat_e7) * 1e-7 * kPi / 180.0;
    const double dx = (b.lon_e7 - a.lon_e7) * 1e-7 * kPi / 180.0 * std::cos(la);
    return kEarthM * std::sqrt(dx * dx + dy * dy);
}

double bearing_deg(Point a, Point b) {
    const double la = (a.lat_e7 + b.lat_e7) * 0.5e-7 * kPi / 180.0;
    const double dy = double(b.lat_e7 - a.lat_e7);
    const double dx = double(b.lon_e7 - a.lon_e7) * std::cos(la);
    double d = std::atan2(dx, dy) * 180.0 / kPi;
    return d < 0 ? d + 360.0 : d;
}

double turn_deg(double from_deg, double to_deg) {
    double d = std::fmod(to_deg - from_deg + 540.0, 360.0) - 180.0;
    return d;
}

const char* to_string(CueKind k) {
    switch (k) {
        case CueKind::Straight:    return "straight";
        case CueKind::SlightLeft:  return "slight left";
        case CueKind::Left:        return "left";
        case CueKind::SharpLeft:   return "sharp left";
        case CueKind::SlightRight: return "slight right";
        case CueKind::Right:       return "right";
        case CueKind::SharpRight:  return "sharp right";
        case CueKind::UTurn:       return "u-turn";
        case CueKind::Arrive:      return "arrive";
        case CueKind::Roundabout:  return "roundabout";
    }
    return "?";
}

const char* to_string(Status s) {
    switch (s) {
        case Status::Ok:        return "ok";
        case Status::NoMap:     return "no map";
        case Status::NoStart:   return "no road near you";
        case Status::NoDest:    return "no road near the destination";
        case Status::NoRoute:   return "no route";
        case Status::TooFar:    return "too far";
        case Status::TooLong:   return "route too long";
        case Status::ReadError: return "map read error";
    }
    return "?";
}

// --- snapping --------------------------------------------------------------------

bool snap(Graph& g, uint32_t variant_flags, Point p, Snap& out, uint32_t max_dm, int ring) {
    double best = max_dm;
    bool found = false;
    bool read_ok = g.near(p.lat_e7, p.lon_e7, ring, [&](uint32_t ai) {
        Attr a;
        if (!g.attr(ai, a)) return false;
        if (!allowed(a.flags, variant_flags)) return true;
        const uint32_t n = a.geom_n + 2u;
        Point prev{}, q{};
        double along = 0, best_along = 0, best_d = 1e18;
        Point best_at{};
        for (uint32_t k = 0; k < n; ++k) {
            const bool r = k == 0 ? g.node(a.from, q)
                         : k == n - 1 ? g.node(a.to, q)
                         : g.geom(a.geom_off + k - 1, q);
            if (!r) return false;
            if (k) {
                // Project p on prev->q in local metres.
                const double seg = dist_m(prev, q);
                const double cl = std::cos(p.lat_e7 * 1e-7 * kPi / 180.0);
                const double ax = (prev.lon_e7 - p.lon_e7) * cl, ay = prev.lat_e7 - p.lat_e7;
                const double bx = (q.lon_e7 - p.lon_e7) * cl,    by = q.lat_e7 - p.lat_e7;
                const double dx = bx - ax, dy = by - ay;
                const double l2 = dx * dx + dy * dy;
                double t = l2 > 0 ? -(ax * dx + ay * dy) / l2 : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                Point at{prev.lat_e7 + int32_t(std::lround((q.lat_e7 - prev.lat_e7) * t)),
                         prev.lon_e7 + int32_t(std::lround((q.lon_e7 - prev.lon_e7) * t))};
                const double d = dist_m(p, at) * 10.0;
                if (d < best_d) {
                    best_d = d;
                    best_along = along + seg * 10.0 * t;
                    best_at = at;
                }
                along += seg * 10.0;
            }
            prev = q;
        }
        if (best_d < best) {
            best = best_d;
            found = true;
            out.attr = ai;
            out.along_dm = uint32_t(best_along + 0.5);
            out.length_dm = uint32_t(along + 0.5);
            if (out.along_dm > out.length_dm) out.along_dm = out.length_dm;
            out.off_dm = uint32_t(best_d + 0.5);
            out.at = best_at;
        }
        return true;
    });
    return read_ok && found;
}

// --- the query ---------------------------------------------------------------------

Status query(Graph& g, uint32_t vf, Point from, Point to, SearchMem mem, Route& out,
             QueryStats* stats) {
    out.clear();
    if (!g.is_open() || g.variant(vf) < 0) return Status::NoMap;
    Snap s, t;
    const uint32_t errors = g.cache().errors();
    const bool found_s = snap(g, vf, from, s);
    const bool found_t = found_s && snap(g, vf, to, t);
    // Avoiding 65 mph roads while on one: a slow road nearby (a frontage
    // road, a parallel street) is not where the rider is, and a route from
    // it would leave them off the route until they found it. No start, so
    // the navigator routes from the 65 mph road itself (Navigator::answered).
    Snap any;
    if (found_s && (vf & kNoFast) && snap(g, vf & ~kNoFast, from, any, kOnFastDm) &&
        any.off_dm < s.off_dm) {
        Attr a;
        if (g.attr(any.attr, a) && (a.flags & kAttrFast)) return Status::NoStart;
    }
    if (g.cache().errors() != errors) return Status::ReadError;
    if (!found_s) return Status::NoStart;
    if (!found_t) return Status::NoDest;
    return query(g, vf, s, t, mem, out, stats);
}

Status query(Graph& g, uint32_t vf, const Snap& s, const Snap& t, SearchMem mem, Route& out,
             QueryStats* stats) {
    out.clear();
    out.variant_flags = vf;
    const int v = g.variant(vf);
    if (!g.is_open() || v < 0) return Status::NoMap;
    const uint32_t misses0 = g.cache().misses();
    Attr A, B;
    if (!g.attr(s.attr, A) || !g.attr(t.attr, B)) return Status::ReadError;
    const bool a_ow = A.flags & kAttrOneway, b_ow = B.flags & kAttrOneway;

    Side f{mem.fwd}, b{mem.bwd};
    f.clear();
    b.clear();
    // Aux values that mark a seed: the start or end attr itself.
    constexpr uint32_t kSeed = 0x7FFFFFFFu;
    f.reach(A.to, part(A.weight_ds, s.length_dm - s.along_dm, s.length_dm), kNone, kSeed);
    // Against a oneway only from its very end: standing on the junction.
    if (!a_ow || s.along_dm == 0) f.reach(A.from, part(A.weight_ds, s.along_dm, s.length_dm), kNone, kSeed);
    b.reach(B.from, part(B.weight_ds, t.along_dm, t.length_dm), kNone, kSeed);
    if (!b_ow || t.along_dm == t.length_dm)
        b.reach(B.to, part(B.weight_ds, t.length_dm - t.along_dm, t.length_dm), kNone, kSeed);

    uint32_t best = kInf, meet = kNone;
    bool direct = false;
    if (s.attr == t.attr && (t.along_dm >= s.along_dm || !a_ow)) {
        const uint32_t d = t.along_dm >= s.along_dm ? t.along_dm - s.along_dm : s.along_dm - t.along_dm;
        best = part(A.weight_ds, d, s.length_dm);
        direct = true;
    }

    uint32_t settled = 0;
    for (;;) {
        const uint32_t tf = f.top(), tb = b.top();
        const bool go_f = tf < best, go_b = tb < best;
        if (!go_f && !go_b) break;
        Side& side = (go_f && (!go_b || tf <= tb)) ? f : b;
        Side& other = &side == &f ? b : f;
        const uint32_t dir = &side == &f ? kEdgeUp : kEdgeDown;
        const HeapItem h = side.pop();
        SearchEntry* e = side.find(h.node);
        e->settled = 1;
        ++settled;
        if (const SearchEntry* o = other.find(h.node)) {
            if (o->dist != kInf && uint64_t(e->dist) + o->dist < best) {
                best = e->dist + o->dist;
                meet = h.node;
                direct = false;
            }
        }
        uint32_t first, last;
        if (!g.edge_range(v, h.node, first, last)) return Status::ReadError;
        for (uint32_t i = first; i < last; ++i) {
            Edge ed;
            if (!g.edge(v, i, ed)) return Status::ReadError;
            if (!(ed.weight_dir & dir)) continue;
            const uint32_t nd = h.dist + (ed.weight_dir & kWeightMask);
            if (!side.reach(ed.target, nd, h.node, ed.aux)) return Status::TooFar;
        }
    }
    if (stats) {
        stats->settled = settled;
        stats->weight_ds = best;
    }
    if (best == kInf) return Status::NoRoute;

    // The packed path: seed -> meet from the forward side, meet -> seed from the backward.
    uint32_t n = 0;
    uint32_t start_node = kNone, end_node = kNone;
    if (!direct) {
        for (uint32_t x = meet;;) {
            const SearchEntry* e = f.find(x);
            if (e->parent == kNone) { start_node = x; break; }
            if (n == mem.hops_cap) return Status::TooLong;
            mem.path[n++] = {e->parent, x, e->aux};
            x = e->parent;
        }
        for (uint32_t i = 0; i < n / 2; ++i) {
            const Hop tmp = mem.path[i];
            mem.path[i] = mem.path[n - 1 - i];
            mem.path[n - 1 - i] = tmp;
        }
        for (uint32_t x = meet;;) {
            const SearchEntry* e = b.find(x);
            if (e->parent == kNone) { end_node = x; break; }
            if (n == mem.hops_cap) return Status::TooLong;
            mem.path[n++] = {x, e->parent, e->aux};
            x = e->parent;
        }
    }
    if (stats) stats->hops = n;

    Builder bld(g, out);
    uint32_t legs = 0;
    if (direct) {
        const bool rev = t.along_dm < s.along_dm;
        bld.add({s.attr, rev, rev ? s.length_dm - s.along_dm : s.along_dm,
                 rev ? s.length_dm - t.along_dm : t.along_dm});
        legs = 1;
    } else {
        // The start road, from the snapped point to the first junction.
        const bool rev0 = start_node == A.from;
        bld.add({s.attr, rev0, rev0 ? s.length_dm - s.along_dm : s.along_dm, kInf});
        ++legs;
        // Unpack every hop, depth first, a -> mid before mid -> b.
        uint32_t sp = 0;
        for (uint32_t i = n; i-- > 0;) {
            if (sp == mem.hops_cap) return Status::TooLong;
            mem.stack[sp++] = mem.path[i];
        }
        while (sp && bld.ok()) {
            const Hop h = mem.stack[--sp];
            if (!(h.aux & kAuxShortcut)) {
                Attr a;
                if (!g.attr(h.aux, a)) return Status::ReadError;
                bld.add({h.aux, a.from != h.a, 0, kInf});
                ++legs;
                continue;
            }
            const uint32_t mid = h.aux & ~kAuxShortcut;
            // Both halves are stored at `mid`, the lowest of the three.
            uint32_t first, last;
            if (!g.edge_range(v, mid, first, last)) return Status::ReadError;
            uint32_t aux1 = kNone, aux2 = kNone, w1 = kInf, w2 = kInf;
            for (uint32_t i = first; i < last; ++i) {
                Edge ed;
                if (!g.edge(v, i, ed)) return Status::ReadError;
                const uint32_t w = ed.weight_dir & kWeightMask;
                if (ed.target == h.a && (ed.weight_dir & kEdgeDown) && w < w1) { w1 = w; aux1 = ed.aux; }
                if (ed.target == h.b && (ed.weight_dir & kEdgeUp) && w < w2) { w2 = w; aux2 = ed.aux; }
            }
            if (aux1 == kNone || aux2 == kNone || sp + 2 > mem.hops_cap) return Status::ReadError;
            mem.stack[sp++] = {mid, h.b, aux2};
            mem.stack[sp++] = {h.a, mid, aux1};
        }
        // The end road, from the last junction to the snapped point.
        const bool revN = end_node == B.to;
        bld.add({t.attr, revN, 0, revN ? t.length_dm - t.along_dm : t.along_dm});
        ++legs;
    }
    bld.finish();
    if (stats) {
        stats->legs = legs;
        stats->cache_misses = g.cache().misses() - misses0;
    }
    return bld.ok() ? Status::Ok : bld.status();
}

size_t longest_roads(const Route& r, RoadLabel* out, size_t max) {
    struct Road { uint32_t name_off, total_ds, best_ds, best_mid; };
    // shortcut: 32 distinct roads tracked; past that a new one replaces the
    // least-used only if its stretch is longer, which can miss a road ridden
    // in many short pieces on a long route. Upgrade if labels look wrong there.
    constexpr size_t kRoads = 32;
    Road roads[kRoads];
    size_t n = 0;
    for (uint32_t i = 0; i + 1 < r.n_pts;) {
        // Segments i..j-1 are one stretch of one road.
        const uint32_t off = r.pts[i].name_off;
        uint32_t j = i + 1;
        while (j + 1 < r.n_pts && r.pts[j].name_off == off) ++j;
        const uint32_t t0 = r.pts[i].time_ds;
        const uint32_t dt = r.pts[j].time_ds - t0;
        if (off != 0 && dt > 0) {
            uint32_t mid = i;
            while (mid < j && r.pts[mid].time_ds - t0 < dt / 2) ++mid;
            size_t k = 0;
            while (k < n && roads[k].name_off != off) ++k;
            if (k == n) {
                if (n < kRoads) {
                    roads[n++] = Road{off, 0, 0, mid};
                } else {
                    size_t least = 0;
                    for (size_t q = 1; q < n; ++q)
                        if (roads[q].total_ds < roads[least].total_ds) least = q;
                    if (roads[least].total_ds >= dt) { i = j; continue; }
                    k = least;
                    roads[k] = Road{off, 0, 0, mid};
                }
            }
            roads[k].total_ds += dt;
            if (dt > roads[k].best_ds) { roads[k].best_ds = dt; roads[k].best_mid = mid; }
        }
        i = j;
    }
    const uint32_t tenth = r.n_pts ? (r.pts[r.n_pts - 1].time_ds - r.pts[0].time_ds) / 10 : 0;
    size_t got = 0;
    while (got < max) {
        size_t best = n;
        for (size_t k = 0; k < n; ++k)
            if (roads[k].total_ds && (best == n || roads[k].total_ds > roads[best].total_ds)) best = k;
        if (best == n || (got >= 2 && roads[best].total_ds < tenth)) break;
        out[got++] = RoadLabel{roads[best].name_off, roads[best].total_ds, roads[best].best_mid};
        roads[best].total_ds = 0;
    }
    return got;
}

}  // namespace core::route
