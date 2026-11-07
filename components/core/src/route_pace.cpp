#include "core/route/pace.hpp"

#include <cmath>
#include <cstdio>

namespace core::route {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint8_t kMaxHalf = 8;
// A bend's curvature, sample by sample, to compare its entry with its exit
// (640 m; a longer bend is neither tightening nor opening).
constexpr uint32_t kMaxBend = 128;

float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// Is a bend whose sharpest point is at `apex_dm` the turn a cue already
// gives (any cue but straight on or arrive)?
bool at_cue(const Route& r, uint32_t apex_dm, uint32_t near_dm) {
    for (uint32_t i = 0; i < r.n_cues; ++i) {
        const Cue& c = r.cues[i];
        if (c.kind == CueKind::Straight || c.kind == CueKind::Arrive) continue;
        const uint32_t d = c.dist_dm > apex_dm ? c.dist_dm - apex_dm : apex_dm - c.dist_dm;
        if (d <= near_dm) return true;
    }
    return false;
}

struct Bend {
    bool     open = false;
    int8_t   dir = 0;
    uint32_t start_dm = 0;
    uint32_t end_dm = 0;
    uint32_t apex_dm = 0;
    float    kmax = 0;
    float    turn = 0;    // radians, signed
    uint32_t n = 0;       // samples in it
};

}  // namespace

uint8_t pace_grade(float r) {
    return r < 20 ? 1 : r < 40 ? 2 : r < 70 ? 3 : r < 110 ? 4 : r < 170 ? 5 : 6;
}

size_t pace_notes(const Route& r, uint32_t from_dm, uint32_t to_dm, PaceNote* out, size_t cap,
                  const PaceConfig& cfg) {
    if (r.n_pts < 2 || cap == 0 || to_dm <= from_dm) return 0;
    const uint8_t half = cfg.half < 1 ? 1 : cfg.half > kMaxHalf ? kMaxHalf : cfg.half;
    const uint32_t step = cfg.step_dm ? cfg.step_dm : 50;
    const float step_m = step / 10.0f;
    const uint32_t end_route = r.pts[r.n_pts - 1].dist_dm;
    // Start far enough back to see a bend already under way, and run on far
    // enough to finish one that starts before to_dm.
    constexpr uint32_t kLeadDm = 3000;
    const uint32_t s0 = from_dm > kLeadDm ? from_dm - kLeadDm : 0;
    const uint32_t s1 = to_dm + kLeadDm < end_route ? to_dm + kLeadDm : end_route;

    // The segment holding s0: the last point at or before it.
    uint32_t lo = 0, hi = r.n_pts - 1;
    while (lo + 1 < hi) {
        const uint32_t mid = (lo + hi) / 2;
        (r.pts[mid].dist_dm <= s0 ? lo : hi) = mid;
    }
    uint32_t seg = lo;

    // Local metres about the first point: a window is a few km, so flat.
    const Point o = r.pts[seg].p;
    const float kx = 0.011132f * std::cos(o.lat_e7 * 1e-7f * kPi / 180.0f);   // m per 1e-7 deg of lon
    const float ky = 0.011132f;

    float rx[2 * kMaxHalf + 1], ry[2 * kMaxHalf + 1];
    uint32_t rs[2 * kMaxHalf + 1];
    const uint32_t ring = 2u * half + 1;
    uint32_t n = 0;     // samples taken

    Bend b;
    float bk[kMaxBend];
    uint32_t last_end = 0;
    bool have_last = false;
    size_t written = 0;

    const auto close = [&]() {
        if (!b.open) return;
        b.open = false;
        const float deg = std::fabs(b.turn) * 180.0f / kPi;
        if (deg < cfg.min_deg) return;
        if (at_cue(r, b.apex_dm, cfg.cue_dm)) return;
        PaceNote p;
        p.at_dm = b.start_dm;
        p.len_dm = b.end_dm > b.start_dm ? b.end_dm - b.start_dm : step;
        p.deg = static_cast<uint16_t>(deg + 0.5f);
        p.dir = b.dir;
        p.grade = pace_grade(1.0f / b.kmax);
        if (have_last && b.start_dm <= last_end + cfg.into_dm) p.flags |= kPaceInto;
        if (deg >= 150 && p.grade <= 2) p.flags |= kPaceHairpin;
        if (p.len_dm >= 1500 && deg >= 60) p.flags |= kPaceLong;
        if (p.len_dm >= 400 && b.n <= kMaxBend) {
            // Entry against exit: the mean curvature of the first and last thirds.
            const uint32_t third = b.n / 3;
            float in = 0, outk = 0;
            for (uint32_t i = 0; i < third; ++i) {
                in += bk[i];
                outk += bk[b.n - 1 - i];
            }
            if (outk > 1.4f * in) p.flags |= kPaceTightens;
            else if (in > 1.4f * outk) p.flags |= kPaceOpens;
        }
        have_last = true;
        last_end = b.end_dm;
        if (b.end_dm >= from_dm && b.start_dm < to_dm && written < cap) out[written++] = p;
    };

    for (uint32_t s = s0; s <= s1; s += step) {
        while (seg + 2 < r.n_pts && r.pts[seg + 1].dist_dm <= s) ++seg;
        const RoutePoint& a = r.pts[seg];
        const RoutePoint& c = r.pts[seg + 1];
        const uint32_t span = c.dist_dm > a.dist_dm ? c.dist_dm - a.dist_dm : 1;
        float t = float(s - a.dist_dm) / float(span);
        if (t > 1) t = 1;
        const float lat = a.p.lat_e7 + t * float(c.p.lat_e7 - a.p.lat_e7);
        const float lon = a.p.lon_e7 + t * float(c.p.lon_e7 - a.p.lon_e7);
        const uint32_t k = n % ring;
        rx[k] = (lon - o.lon_e7) * kx;
        ry[k] = (lat - o.lat_e7) * ky;
        rs[k] = s;
        ++n;
        if (n < ring) continue;

        // The ring's oldest, middle and newest samples.
        const uint32_t i0 = n % ring, im = (n + half) % ring, i1 = (n - 1) % ring;
        const float h1 = std::atan2(rx[im] - rx[i0], ry[im] - ry[i0]);
        const float h2 = std::atan2(rx[i1] - rx[im], ry[i1] - ry[im]);
        const float curv = wrap(h2 - h1) / (half * step_m);   // + right
        const uint32_t at = rs[im];
        const float mag = std::fabs(curv);
        const int8_t sgn = curv > 0 ? 1 : -1;

        if (b.open && (curv * b.dir < cfg.exit_per_m || (sgn != b.dir && mag >= cfg.enter_per_m))) close();
        if (!b.open && mag >= cfg.enter_per_m) {
            b = Bend{};
            b.open = true;
            b.dir = sgn;
            b.start_dm = at;
        }
        if (b.open) {
            if (b.n < kMaxBend) bk[b.n] = mag;
            ++b.n;
            b.end_dm = at;
            b.turn += curv * step_m;
            if (mag > b.kmax) {
                b.kmax = mag;
                b.apex_dm = at;
            }
        }
        if (at > s1) break;
    }
    close();
    return written;
}

size_t format_note(const PaceNote& n, char* out, size_t cap) {
    const char* tail = (n.flags & kPaceHairpin) ? " hairpin"
                     : (n.flags & kPaceTightens) ? " tightens"
                     : (n.flags & kPaceOpens) ? " opens" : "";
    const char* lng = (n.flags & kPaceLong) && !(n.flags & kPaceHairpin) ? " long" : "";
    const int w = std::snprintf(out, cap, "%s%c%u%s%s", (n.flags & kPaceInto) ? "into " : "",
                                n.dir > 0 ? 'R' : 'L', unsigned(n.grade), lng, tail);
    return w < 0 ? 0 : (size_t(w) < cap ? size_t(w) : cap - 1);
}

}  // namespace core::route
