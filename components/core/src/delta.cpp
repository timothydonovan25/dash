#include "core/delta/delta.hpp"
#include <cmath>

namespace core::delta {
namespace {

inline float dist_sq(const Point& p, float x, float y) {
    const float dx = p.x_ft - x;
    const float dy = p.y_ft - y;
    return dx * dx + dy * dy;
}

// (x, y) projected onto the segment a->b: how far along it (0..1), and the
// squared distance from the segment. A degenerate segment projects to a.
float project(const Point& a, const Point& b, float x, float y, float& d2) {
    const float sx = b.x_ft - a.x_ft;
    const float sy = b.y_ft - a.y_ft;
    const float len2 = sx * sx + sy * sy;
    float u = 0.0f;
    if (len2 > 0.0f) {
        u = ((x - a.x_ft) * sx + (y - a.y_ft) * sy) / len2;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    }
    const float px = a.x_ft + u * sx - x;
    const float py = a.y_ft + u * sy - y;
    d2 = px * px + py * py;
    return u;
}

int32_t quantise(int32_t ms, uint32_t quantum) {
    if (quantum <= 1) return ms;
    const int32_t q = static_cast<int32_t>(quantum);
    // Round half away from zero, so a delta of exactly +50 ms shows as +0.1
    // rather than 0.0 — the sign is the part a rider reads first.
    return ms >= 0 ? ((ms + q / 2) / q) * q : -(((-ms + q / 2) / q) * q);
}

}  // namespace

void Tracker::start_lap() {
    rec_count_ = 0;
    match_ = 0;
    anchored_ = false;
    valid_ = false;
    delta_ms_ = 0;
}

void Tracker::clear() {
    rec_count_ = 0;
    ref_count_ = 0;
    match_ = 0;
    anchored_ = false;
    valid_ = false;
    delta_ms_ = 0;
    last_match_ft_ = 0.0f;
}

void Tracker::finish_lap(bool is_best) {
    // Only a lap the caller calls best becomes the reference. The lap timer
    // already refuses to let a lap with a GPS gap in it set the best, so a
    // dropout cannot poison the reference either.
    if (is_best && rec_count_ > 1) {
        for (size_t i = 0; i < rec_count_; ++i) ref_[i] = rec_[i];
        ref_count_ = rec_count_;
    }
    rec_count_ = 0;
}

void Tracker::sample(float x_ft, float y_ft, uint32_t elapsed_ms,
                     const Config& c) {
    if (rec_count_ < kMaxPoints) {
        Point& p = rec_[rec_count_++];
        p.x_ft = x_ft;
        p.y_ft = y_ft;
        p.elapsed_ms = elapsed_ms;
    }

    if (ref_count_ < 2) {
        valid_ = false;
        return;
    }

    // The first sample of a lap has no anchor, so it searches everything.
    // After that the window is what keeps the match honest where a circuit
    // runs near itself — a hairpin, or a return road beside the straight.
    // A global nearest-point search would happily jump to the other branch
    // and report a delta of half a lap.
    size_t first = 0;
    size_t last = ref_count_ - 1;
    if (anchored_) {
        first = match_ > c.search_back ? match_ - c.search_back : 0;
        const size_t ahead = match_ + c.search_forward;
        last = ahead < ref_count_ - 1 ? ahead : ref_count_ - 1;
    }

    size_t best_i = first;
    float  best_d2 = dist_sq(ref_[first], x_ft, y_ft);
    for (size_t i = first + 1; i <= last; ++i) {
        const float d2 = dist_sq(ref_[i], x_ft, y_ft);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_i = i;
        }
    }

    // The comparison below is done squared; the root is taken once, only so
    // the diag screen can show a distance a human recognises.
    last_match_ft_ = std::sqrt(best_d2);

    if (best_d2 > c.max_match_ft * c.max_match_ft) {
        // Too far from anything on the reference line. Keep the anchor —
        // riding through the pits and rejoining should pick up again rather
        // than re-searching the whole lap — but publish nothing.
        valid_ = false;
        return;
    }

    match_ = best_i;
    anchored_ = true;

    // Along the reference line, not at its nearest stored point: the rider
    // is usually between two of them, and snapping to one was up to half a
    // sample interval (±50 ms at 10 Hz) of error before any rounding. Of
    // the two segments either side of the nearest point, the closer one.
    float ref_ms = static_cast<float>(ref_[best_i].elapsed_ms);
    float seg_best = best_d2;
    for (int side = -1; side <= 1; side += 2) {
        const size_t j = side < 0 ? (best_i > 0 ? best_i - 1 : best_i) : best_i + 1;
        if (j == best_i || j >= ref_count_) continue;
        const Point& a = side < 0 ? ref_[j] : ref_[best_i];
        const Point& b = side < 0 ? ref_[best_i] : ref_[j];
        float d2 = 0.0f;
        const float u = project(a, b, x_ft, y_ft, d2);
        if (d2 < seg_best) {
            seg_best = d2;
            ref_ms = static_cast<float>(a.elapsed_ms) +
                     u * (static_cast<float>(b.elapsed_ms) - static_cast<float>(a.elapsed_ms));
        }
    }
    const int32_t raw = static_cast<int32_t>(elapsed_ms) -
                        static_cast<int32_t>(std::lround(ref_ms));
    delta_ms_ = quantise(raw, c.quantum_ms);
    valid_ = true;
}

bool Tracker::delta_ms(int32_t& out) const {
    out = valid_ ? delta_ms_ : 0;
    return valid_;
}

}  // namespace core::delta
