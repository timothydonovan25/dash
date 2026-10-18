#include "core/lap/sectors.hpp"
#include <cmath>

namespace core::lap {

uint32_t Sectors::ideal_ms() const {
    uint32_t sum = 0;
    for (int k = 0; k < kSectors; ++k) {
        if (best_[k] == 0) return 0;
        sum += best_[k];
    }
    return sum;
}

void Sectors::reset() {
    // Field by field: a whole Sectors is 4 kB of path, and `*this =
    // Sectors{}` builds one on the logic task's stack to copy from.
    path_n_ = 0;
    path_stride_ = 1;
    path_skip_ = 0;
    path_dist_ft_ = 0.0f;
    recording_ = false;
    placed_ = false;
    timing_ = false;
    current_ = 0;
    gapped_ = false;
    has_prev_ = false;
    for (uint32_t& b : best_) b = 0;
}

void Sectors::place_lines(const geo::LatLon (&at)[kSectors - 1],
                          const double (&heading_deg)[kSectors - 1]) {
    for (int k = 0; k < kSectors - 1; ++k) {
        lines_[k] = at[k];
        headings_[k] = heading_deg[k];
    }
    placed_ = true;
    recording_ = false;
    path_n_ = 0;
}

void Sectors::abort_lap() {
    timing_ = false;
    recording_ = false;
    path_n_ = 0;
}

void Sectors::start_lap(Instant at) {
    timing_ = placed_;
    current_ = 0;
    sector_start_ = at;
    gapped_ = false;
    // Until there are lines, every lap is recorded so the first clean one
    // can place them.
    recording_ = !placed_;
    path_n_ = 0;
    path_stride_ = 1;
    path_skip_ = 0;
    path_dist_ft_ = 0.0f;
}

void Sectors::record(const Fix& f, float course_deg, bool segment_ok) {
    if (!recording_) return;
    if (path_n_ == 0) {
        path_origin_ = f.pos;
        path_prev_ = f.pos;
    } else if (segment_ok) {
        path_dist_ft_ += static_cast<float>(geo::distance_ft(path_prev_, f.pos));
        path_prev_ = f.pos;
    } else {
        // A hole in the lap: its distance is unknown, so it cannot place
        // lines. The Timer will not score it as clean either.
        recording_ = false;
        path_n_ = 0;
        return;
    }
    if (path_skip_ > 0) {
        --path_skip_;
        return;
    }
    path_skip_ = path_stride_ - 1;
    if (path_n_ == kPath) {
        // Full: keep every other point and take half as many from here on.
        // A two-minute lap at 10 Hz ends up a point every half second or
        // so, plenty to find a third of the way round.
        for (size_t i = 0; i < kPath / 2; ++i) path_[i] = path_[2 * i];
        path_n_ = kPath / 2;
        path_stride_ *= 2;
        path_skip_ = path_stride_ - 1;
    }
    PathPoint p;
    p.dlat = static_cast<float>(f.pos.lat - path_origin_.lat);
    p.dlon = static_cast<float>(f.pos.lon - path_origin_.lon);
    p.dist_ft = path_dist_ft_;
    p.course_deg = course_deg;
    path_[path_n_++] = p;
}

void Sectors::place() {
    if (path_n_ < 2 * kSectors) return;
    const float total = path_dist_ft_;
    if (total <= 0.0f) return;
    size_t i = 0;
    for (int k = 1; k < kSectors; ++k) {
        const float target = total * static_cast<float>(k) / kSectors;
        while (i + 1 < path_n_ && path_[i].dist_ft < target) ++i;
        // Between the stored points either side of the target: they can be
        // half a second apart once the path has been thinned.
        float t = 1.0f;
        if (i > 0 && path_[i].dist_ft > path_[i - 1].dist_ft) {
            t = (target - path_[i - 1].dist_ft) / (path_[i].dist_ft - path_[i - 1].dist_ft);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
        }
        const PathPoint& a = path_[i > 0 ? i - 1 : 0];
        const PathPoint& b = path_[i];
        lines_[k - 1] = geo::LatLon{path_origin_.lat + a.dlat + t * (b.dlat - a.dlat),
                                    path_origin_.lon + a.dlon + t * (b.dlon - a.dlon)};
        headings_[k - 1] = t < 0.5f ? a.course_deg : b.course_deg;
    }
    placed_ = true;
}

void Sectors::close_sector(Instant at, Split& out) {
    const double span = static_cast<double>(at.ms - sector_start_.ms) +
                        (at.frac - sector_start_.frac);
    const uint32_t ms = span <= 0.0 ? 0u : static_cast<uint32_t>(span + 0.5);
    const int k = current_;
    out.crossed = true;
    out.sector = static_cast<uint8_t>(k);
    out.ms = ms;
    out.at_ms = at.ms;
    if (!gapped_ && best_[k] != 0) {
        out.delta_ms = static_cast<int32_t>(ms) - static_cast<int32_t>(best_[k]);
        out.delta_valid = true;
    }
    if (!gapped_ && (best_[k] == 0 || ms < best_[k])) best_[k] = ms;
    sector_start_ = at;
    gapped_ = false;
}

Split Sectors::update(const Fix& f, float course_deg, const Update& lap, const Config& c) {
    Split out;

    if (!f.valid) {
        if (has_prev_ && timing_) gapped_ = true;
        has_prev_ = false;
        if (recording_) {
            recording_ = false;
            path_n_ = 0;
        }
        return out;
    }

    const uint32_t dt_ms = f.t_ms - prev_t_ms_;
    const bool segment_ok = has_prev_ && dt_ms <= c.max_fix_gap_ms;
    if (has_prev_ && !segment_ok && timing_) gapped_ = true;

    if (lap.event == Event::Started || lap.event == Event::Completed) {
        const Instant at{lap.at_ms, lap.at_frac};
        if (lap.event == Event::Completed) {
            // The last sector ends at the line. A lap that missed a split
            // has no honest time for the sectors after it: nothing kept.
            if (timing_ && current_ == kSectors - 1) close_sector(at, out);
            // The first clean lap places the splits; the next one is timed.
            if (!placed_ && recording_ && lap.valid) {
                record(f, course_deg, segment_ok);
                place();
            }
        }
        start_lap(at);
        record(f, course_deg, segment_ok);
    } else {
        record(f, course_deg, segment_ok);
        if (timing_ && current_ < kSectors - 1 && segment_ok &&
            f.speed_mph >= c.min_speed_mph) {
            const geo::Crossing x = geo::crosses_line(prev_, f.pos, lines_[current_],
                                                      headings_[current_], c.half_width_ft);
            if (x.crossed) {
                const double offset_ms = static_cast<double>(dt_ms) * x.fraction;
                const double whole = std::floor(offset_ms);
                Instant at;
                at.ms = prev_t_ms_ + static_cast<uint32_t>(whole);
                at.frac = static_cast<float>(offset_ms - whole);
                close_sector(at, out);
                ++current_;
            }
        }
    }

    prev_ = f.pos;
    prev_t_ms_ = f.t_ms;
    has_prev_ = true;
    return out;
}

void Peaks::add(float mph_now, float rpm_now, float lean_deg_now) {
    if (mph_now > mph) mph = mph_now;
    if (rpm_now > rpm) rpm = rpm_now;
    if (mph_now >= kMinMph) {
        if (-lean_deg_now > lean_left_deg) lean_left_deg = -lean_deg_now;
        if (lean_deg_now > lean_right_deg) lean_right_deg = lean_deg_now;
    }
}

}  // namespace core::lap
