#include "core/stunt/stunt.hpp"

#include <cmath>

namespace core::stunt {
namespace {

constexpr float kDeg2Rad = 0.017453293f;
constexpr float kRad2Deg = 57.29578f;
constexpr float kG = 9.80665f;
constexpr float kMpsPerMph = 0.44704f;
constexpr float kFtPerSPerMph = 1.4666667f;
// A pass further apart than this is a gap, not a step: start the clock and
// the climb filter over rather than integrate across it.
constexpr uint32_t kMaxStepMs = 200;

float fabsf_(float v) { return v < 0.0f ? -v : v; }

uint8_t grade_for(uint8_t kind, uint32_t ms) {
    if (kind == Looped) return 0;
    if (kind == Stoppie) return ms < 1000 ? 0 : ms < 2000 ? 1 : 2;
    return ms < 1500 ? 0 : ms < 3000 ? 1 : ms < 6000 ? 2 : ms < 10000 ? 3 : 4;
}

float combo_mult(uint8_t combo) {
    const float m = 1.0f + 0.5f * static_cast<float>(combo > 0 ? combo - 1 : 0);
    return m > 4.0f ? 4.0f : m;
}

}  // namespace

const char* grade_name(uint8_t kind, uint8_t grade) {
    static const char* const kWheelie[] = {"POP", "WHEELIE", "NICE ONE", "SICK", "LEGENDARY"};
    static const char* const kStoppie[] = {"STOPPIE", "BIG STOPPIE", "ENDO MASTER"};
    switch (kind) {
        case Wheelie: return grade < 5 ? kWheelie[grade] : "WHEELIE";
        case Stoppie: return grade < 3 ? kStoppie[grade] : "STOPPIE";
        case Looped:  return "LOOPED OUT";
        default:      return "";
    }
}

const char* landing_name(uint8_t landing) {
    switch (landing) {
        case Smooth:  return "SMOOTH";
        case Slammed: return "SLAMMED";
        default:      return "";
    }
}

void Scorer::reset_session() {
    const state::Stunt keep = out_;
    out_ = state::Stunt{};
    out_.seq = keep.seq;   // a reset is not a new trick
    combo_ = 0;
    combo_live_ = false;
}

float Scorer::rate(float lift_abs, float mph, bool balance, const Config& c) const {
    const float base = kind_ == Stoppie ? c.stoppie_pts_per_s : c.wheelie_pts_per_s;
    // From 10°, not start_deg: raising the detection threshold must not cheapen points.
    float angle = 1.0f + (lift_abs - 10.0f) / 20.0f;
    if (angle < 1.0f) angle = 1.0f;
    return base * angle * (1.0f + mph / 40.0f) * (balance ? 1.5f : 1.0f);
}

void Scorer::climb_update(const Input& in, float dt, const Config& c) {
    // The specific force in the bike's frame, turned into the world's
    // vertical: nose-up pitch puts some of x on "up", lean takes some of z
    // off it. At rest this is 1 g, whatever the attitude.
    const float th = in.pitch_deg * kDeg2Rad;
    const float ph = in.lean_deg * kDeg2Rad;
    const float f_up = in.g_lon * std::sin(th) +
                       std::cos(th) * (in.g_lat * std::sin(ph) + in.g_vert * std::cos(ph));
    const float a_up = (f_up - 1.0f) * kG;

    // In the air the accelerometer reads free fall, and integrating it
    // would land the bike on a phantom descent. Start again from GPS, with
    // no climb, once it is back on the ground.
    if (in.airborne) {
        climb_init_ = false;
        vz_mps_ = 0.0f;
        ground_deg_ = 0.0f;
        return;
    }
    if (!climb_init_) {
        if (!in.gps_fix) {
            vz_mps_ = 0.0f;
            ground_deg_ = 0.0f;
            return;
        }
        climb_init_ = true;
        h_m_ = in.gps_alt_m;
        vz_mps_ = 0.0f;
    }

    vz_mps_ += a_up * dt;
    h_m_ += vz_mps_ * dt;
    if (in.gps_fix) {
        const float w = c.climb_omega;
        const float e = in.gps_alt_m - h_m_;
        h_m_ += 2.0f * w * e * dt;      // critically damped
        vz_mps_ += w * w * e * dt;
    }
    else {
        // Nothing to hold it to: let an unchecked climb rate die away
        // rather than drift. Not with a fix, where it would bias a steady
        // climb low (a 20 degree hill read as 13).
        vz_mps_ -= vz_mps_ * (dt / c.climb_leak_s);
    }

    // The path's angle. A bike standing still is not climbing anything.
    const float v = in.mph * kMpsPerMph;
    if (v < 1.0f) {
        vz_mps_ = 0.0f;
        ground_deg_ = 0.0f;
        return;
    }
    float s = vz_mps_ / v;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    ground_deg_ = std::asin(s) * kRad2Deg;
}

void Scorer::end_trick(uint32_t end_ms, const Config& c) {
    const uint32_t ms = end_ms - start_ms_;
    live_ms_ = 0;
    if (ms < c.min_scored_ms) {
        // A twitch: nothing scored, nothing chained, nothing broken.
        phase_ = Phase::Idle;
        kind_ = None;
        return;
    }
    // land_peak_g_ has been watched since the front came down (below_).
    end_ms_ = end_ms;
    phase_ = Phase::Landing;
}

void Scorer::finalize(const Config& c) {
    state::Stunt& o = out_;
    const uint32_t ms = end_ms_ - start_ms_;
    uint8_t landing = Plain;
    if (land_peak_g_ > c.slam_g) landing = Slammed;
    else if (land_peak_g_ < c.smooth_g) landing = Smooth;
    const float land_mult = landing == Smooth ? 1.2f : landing == Slammed ? 0.7f : 1.0f;

    // Into the combo if it started within the window of the last one.
    if (combo_live_ && start_ms_ - combo_end_ms_ <= kComboWindowMs) {
        if (combo_ < 255) ++combo_;
    } else {
        combo_ = 1;
    }
    combo_live_ = true;
    combo_end_ms_ = end_ms_;
    const float mult = combo_mult(combo_);

    const uint32_t pts = static_cast<uint32_t>(points_ * land_mult * mult + 0.5f);
    o.kind = kind_;
    o.grade = grade_for(kind_, ms);
    o.landing = landing;
    o.ms = ms;
    o.ft = ft_;
    o.peak_deg = peak_deg_;
    o.points = pts;
    o.mult_x10 = static_cast<uint8_t>(mult * 10.0f + 0.5f);
    o.record = pts > o.best_points;
    o.total += pts;
    if (o.tricks < UINT16_MAX) ++o.tricks;
    if (pts > o.best_points) o.best_points = pts;
    if (ms > o.longest_ms) o.longest_ms = ms;
    if (ft_ > o.longest_ft) o.longest_ft = ft_;
    if (kind_ == Wheelie && peak_deg_ > o.highest_deg) o.highest_deg = peak_deg_;
    if (combo_ > o.best_combo) o.best_combo = combo_;
    o.done_ms = now_ms_ != 0 ? now_ms_ : 1;
    ++o.seq;
    if (o.seq == 0) o.seq = 1;

    phase_ = Phase::Idle;
    kind_ = None;
}

void Scorer::update(const Input& in, const Config& c) {
    now_ms_ = in.t_ms;
    if (!have_t_ || in.t_ms - last_t_ > kMaxStepMs) {
        have_t_ = true;
        last_t_ = in.t_ms;
        climb_init_ = false;
        if (phase_ == Phase::Active || phase_ == Phase::Candidate) {
            phase_ = Phase::Idle;
            kind_ = None;
            live_ms_ = 0;
        }
        return;
    }
    const uint32_t dt_ms = in.t_ms - last_t_;
    last_t_ = in.t_ms;
    if (dt_ms == 0) return;
    const float dt = static_cast<float>(dt_ms) / 1000.0f;

    climb_update(in, dt, c);
    lift_deg_ = in.pitch_deg - ground_deg_;

    const bool eligible = in.imu_ok && in.speed_valid && in.mph >= c.min_mph && !in.airborne;
    // The lift as the trick under way measures it: + is "more trick".
    const float signed_lift = kind_ == Stoppie ? -lift_deg_ : lift_deg_;

    switch (phase_) {
        case Phase::Idle:
            if (eligible && fabsf_(lift_deg_) >= c.start_deg) {
                kind_ = lift_deg_ > 0.0f ? Wheelie : Stoppie;
                phase_ = Phase::Candidate;
                start_ms_ = in.t_ms;
                points_ = 0.0f;
                ft_ = 0.0f;
                peak_deg_ = fabsf_(lift_deg_);
                balance_ = false;
                below_ = false;
            }
            break;

        case Phase::Candidate:
        case Phase::Active: {
            if (!eligible) {
                // Airborne or stopped: whatever it was is over, now.
                if (phase_ == Phase::Active) end_trick(in.t_ms, c);
                else { phase_ = Phase::Idle; kind_ = None; }
                break;
            }
            if (kind_ == Wheelie && signed_lift >= c.loop_deg) {
                // Over the top. No points, and the combo goes with it.
                state::Stunt& o = out_;
                o.kind = Looped;
                o.grade = 0;
                o.landing = Plain;
                o.ms = in.t_ms - start_ms_;
                o.ft = ft_;
                o.peak_deg = signed_lift;
                o.points = 0;
                o.mult_x10 = 10;
                o.record = false;
                o.done_ms = in.t_ms != 0 ? in.t_ms : 1;
                ++o.seq;
                if (o.seq == 0) o.seq = 1;
                combo_ = 0;
                combo_live_ = false;
                phase_ = Phase::Idle;
                kind_ = None;
                live_ms_ = 0;
                break;
            }
            if (signed_lift < c.end_deg) {
                if (!below_) {
                    below_ = true;
                    below_since_ = in.t_ms;
                    land_peak_g_ = 0.0f;
                }
                // The landing is from the front coming down, not from when
                // the trick is sure to be over.
                if (in.g_vert > land_peak_g_) land_peak_g_ = in.g_vert;
                if (phase_ == Phase::Candidate) {
                    phase_ = Phase::Idle;
                    kind_ = None;
                } else if (in.t_ms - below_since_ >= c.end_hold_ms) {
                    end_trick(below_since_, c);
                }
                break;
            }
            below_ = false;
            if (signed_lift > peak_deg_) peak_deg_ = signed_lift;
            balance_ = kind_ == Wheelie && signed_lift >= c.balance_lo_deg &&
                       signed_lift <= c.balance_hi_deg;
            points_ += rate(signed_lift, in.mph, balance_, c) * dt;
            ft_ += in.mph * kFtPerSPerMph * dt;
            if (phase_ == Phase::Candidate && in.t_ms - start_ms_ >= c.confirm_ms) {
                phase_ = Phase::Active;
            }
            live_ms_ = phase_ == Phase::Active ? in.t_ms - start_ms_ : 0;
            break;
        }

        case Phase::Landing:
            if (in.g_vert > land_peak_g_) land_peak_g_ = in.g_vert;
            if (in.t_ms - end_ms_ >= c.landing_window_ms) finalize(c);
            break;
    }
}

state::Stunt Scorer::publish() const {
    state::Stunt o = out_;
    const bool live = phase_ == Phase::Active;
    o.live_kind = live ? kind_ : static_cast<uint8_t>(None);
    o.live_ms = live ? live_ms_ : 0;
    o.live_ft = live ? ft_ : 0.0f;
    o.live_deg = lift_deg_;
    o.live_points = live ? points_ : 0.0f;
    o.live_balance = live && balance_;
    // The multiplier the next trick gets, and the chain it would join; a
    // combo whose window has closed shows as none.
    const bool open = combo_live_ && (phase_ == Phase::Active || phase_ == Phase::Landing ||
                                      now_ms_ - combo_end_ms_ <= kComboWindowMs);
    o.combo = open ? combo_ : 0;
    o.combo_x10 = static_cast<uint8_t>(combo_mult(open ? static_cast<uint8_t>(combo_ + 1) : 1) *
                                       10.0f + 0.5f);
    return o;
}

}  // namespace core::stunt
