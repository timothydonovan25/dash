#include "core/lap/lap.hpp"
#include <cmath>

namespace core::lap {

void Timer::set_line(geo::LatLon at, double heading_deg) {
    line_ = at;
    line_heading_deg_ = heading_deg;
    has_line_ = true;
    pub_.phase = Phase::Armed;
    pub_.current_ms = 0;
    gapped_ = false;
    // Deliberately not touching prev_: the rider is usually moving when they
    // set the line, and the very next segment is allowed to score.
}

void Timer::arm() {
    if (!has_line_) return;
    pub_.phase = Phase::Armed;
    pub_.current_ms = 0;
    gapped_ = false;
}

void Timer::clear() {
    has_line_ = false;
    has_prev_ = false;
    gapped_ = false;
    lap_start_ = Instant{};
    pub_ = State{};
}

void Timer::begin_lap(Instant at) {
    lap_start_ = at;
    gapped_ = false;
    pub_.phase = Phase::Timing;
    pub_.current_ms = 0;
}

Update Timer::update(const Fix& f, const Config& c) {
    Update out;

    if (!f.valid) {
        // No fix, so the line cannot be seen. Break the segment: pairing the
        // last known position with wherever the receiver reappears would draw
        // a straight line through everything in between, including the line.
        if (has_prev_) gapped_ = gapped_ || pub_.phase == Phase::Timing;
        has_prev_ = false;
        if (pub_.phase == Phase::Timing) pub_.current_ms = f.t_ms - lap_start_.ms;
        return out;
    }

    if (has_line_) {
        pub_.dist_to_line_ft = static_cast<float>(geo::distance_ft(f.pos, line_));
    }

    // Unsigned subtraction, so a wrapped counter still gives the true gap.
    const uint32_t dt_ms = f.t_ms - prev_t_ms_;
    const bool segment_ok = has_prev_ && dt_ms <= c.max_fix_gap_ms;
    if (has_prev_ && !segment_ok) {
        gapped_ = gapped_ || pub_.phase == Phase::Timing;
    }

    if (has_line_ && segment_ok && f.speed_mph >= c.min_speed_mph) {
        const geo::Crossing x = geo::crosses_line(prev_, f.pos, line_,
                                                  line_heading_deg_,
                                                  c.half_width_ft);
        if (x.crossed) {
            // Interpolate the instant of the cut inside the fix pair.
            const double offset_ms = static_cast<double>(dt_ms) * x.fraction;
            const double whole = std::floor(offset_ms);
            Instant at;
            at.ms   = prev_t_ms_ + static_cast<uint32_t>(whole);
            at.frac = static_cast<float>(offset_ms - whole);

            if (pub_.phase == Phase::Armed) {
                begin_lap(at);
                out.event = Event::Started;
            } else if (pub_.phase == Phase::Timing) {
                const double span = static_cast<double>(at.ms - lap_start_.ms) +
                                    (at.frac - lap_start_.frac);
                const uint32_t lap_ms =
                    span <= 0.0 ? 0u : static_cast<uint32_t>(span + 0.5);

                // Too soon to be a lap: the rider is weaving over the line, or
                // one fix pair spanned it in both directions. Not an event,
                // and not a reason to restart the clock.
                if (lap_ms >= c.min_lap_ms) {
                    out.event  = Event::Completed;
                    out.lap_ms = lap_ms;
                    out.valid  = !gapped_;

                    pub_.last_ms = lap_ms;
                    if (pub_.count != UINT16_MAX) ++pub_.count;
                    if (out.valid && (pub_.best_ms == 0 || lap_ms < pub_.best_ms)) {
                        pub_.best_ms = lap_ms;
                    }
                    begin_lap(at);
                }
            }
        }
    }

    prev_ = f.pos;
    prev_t_ms_ = f.t_ms;
    has_prev_ = true;

    if (pub_.phase == Phase::Timing) pub_.current_ms = f.t_ms - lap_start_.ms;
    return out;
}

}  // namespace core::lap
