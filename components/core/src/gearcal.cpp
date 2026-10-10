#include "core/gearcal/gearcal.hpp"

namespace core::gearcal {
namespace {

inline float abs_(float v) { return v < 0.0f ? -v : v; }

// Median of up to eight values. Insertion sort: it is five comparisons and
// nothing here is worth a library.
float median(const float* v, uint8_t n) {
    if (n == 0) return 0.0f;
    float t[LearnConfig::kRing];
    if (n > LearnConfig::kRing) n = LearnConfig::kRing;
    for (uint8_t i = 0; i < n; ++i) {
        float x = v[i];
        uint8_t j = i;
        while (j > 0 && t[j - 1] > x) { t[j] = t[j - 1]; --j; }
        t[j] = x;
    }
    return (n & 1) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

// The gear whose table entry the ratio is proportionally nearest, 0 with
// none to compare against. Optionally the runner-up's distance too.
uint8_t nearest_gear(float ratio, const float* table, uint8_t count,
                     float* best_err = nullptr, float* second_abs = nullptr) {
    uint8_t g = 0;
    float best = 0.0f, best_abs = 1e9f, second = 1e9f;
    for (uint8_t i = 0; i < count && i < kMaxGears; ++i) {
        if (table[i] <= 0.0f) continue;
        const float e = ratio / table[i] - 1.0f;
        const float a = abs_(e);
        if (a < best_abs) {
            second = best_abs;
            best_abs = a;
            best = e;
            g = static_cast<uint8_t>(i + 1);
        } else if (a < second) {
            second = a;
        }
    }
    if (best_err) *best_err = best;
    if (second_abs) *second_abs = second;
    return g;
}

}  // namespace

const char* reason_text(Reason r) {
    switch (r) {
        case Reason::Steady:    return "steady";
        case Reason::Neutral:   return "in neutral";
        case Reason::Revs:      return "revs too low";
        case Reason::Slow:      return "too slow";
        case Reason::NoGps:     return "no GPS fix";
        case Reason::GpsApart:  return "wheel and GPS differ";
        case Reason::Unsteady:  return "hold speed and revs";
        case Reason::WrongGear: return "wrong gear?";
    }
    return "";
}

// --- Window -------------------------------------------------------------

void Window::clear_block() {
    block_n_ = 0;
    b_rpm_ = b_road_ = b_gps_ = 0.0f;
}

void Window::reset() {
    clear_block();
    blocks_ = 0;
    sum_rpm_ = sum_road_ = sum_gps_ = 0.0f;
    ++gen_;
}

void Window::restart_with(float rpm, float road, float gps) {
    reset();
    blocks_ = 1;
    sum_rpm_ = rpm;
    sum_road_ = road;
    sum_gps_ = gps;
    min_rpm_ = max_rpm_ = rpm;
    min_road_ = max_road_ = road;
    min_ratio_ = max_ratio_ = rpm / road;
}

float Window::ratio() const {
    return sum_road_ > 0.0f && blocks_ ? sum_rpm_ / sum_road_ : 0.0f;
}

float Window::gps_over_road() const {
    return sum_road_ > 0.0f && blocks_ ? sum_gps_ / sum_road_ - 1.0f : 0.0f;
}

Reason Window::feed(const Sample& s, const SteadyConfig& c) {
    block_ms_ = c.block_ms;
    if (have_last_ && s.t_ms - last_t_ms_ > c.max_gap_ms) reset();
    last_t_ms_ = s.t_ms;
    have_last_ = true;

    // Things that are wrong at this instant, and cannot be averaged away.
    Reason bad = Reason::Steady;
    if (s.neutral)                      bad = Reason::Neutral;
    else if (s.rpm < c.min_rpm)         bad = Reason::Revs;
    else if (s.road_mph < c.min_mph)    bad = Reason::Slow;
    else if (!s.gps_ok || s.hdop > c.max_hdop) bad = Reason::NoGps;
    if (bad != Reason::Steady) {
        reset();
        reason_ = bad;
        return bad;
    }
    // Those four clear the moment the sample is good again; the other two
    // wait for the next block to be judged.
    if (reason_ != Reason::GpsApart && reason_ != Reason::Unsteady) {
        reason_ = Reason::Steady;
    }

    if (block_n_ == 0) block_start_ms_ = s.t_ms;
    b_rpm_ += s.rpm;
    b_road_ += s.road_mph;
    b_gps_ += s.gps_mph;
    ++block_n_;
    if (s.t_ms - block_start_ms_ < c.block_ms) return reason_;

    const float n = static_cast<float>(block_n_);
    const float rpm = b_rpm_ / n;
    const float road = b_road_ / n;
    const float gps = b_gps_ / n;
    clear_block();

    const float ref = gps > 1.0f ? gps : 1.0f;
    if (abs_(road - gps) / ref > c.gps_agree) {
        reset();
        reason_ = Reason::GpsApart;
        return reason_;
    }

    const float ratio = rpm / road;
    if (blocks_ == 0) {
        restart_with(rpm, road, gps);
        reason_ = Reason::Steady;
        return reason_;
    }
    ++blocks_;
    sum_rpm_ += rpm;
    sum_road_ += road;
    sum_gps_ += gps;
    if (rpm < min_rpm_) min_rpm_ = rpm;
    if (rpm > max_rpm_) max_rpm_ = rpm;
    if (road < min_road_) min_road_ = road;
    if (road > max_road_) max_road_ = road;
    if (ratio < min_ratio_) min_ratio_ = ratio;
    if (ratio > max_ratio_) max_ratio_ = ratio;

    const float bn = static_cast<float>(blocks_);
    const float mean_rpm = sum_rpm_ / bn;
    const float mean_road = sum_road_ / bn;
    const float mean_ratio = sum_rpm_ / sum_road_;
    if ((max_rpm_ - min_rpm_) > c.rpm_band * mean_rpm ||
        (max_road_ - min_road_) > c.speed_band * mean_road ||
        (max_ratio_ - min_ratio_) > c.ratio_band * mean_ratio) {
        // Moved. What came before is not a steady stretch, but this block
        // may be the start of one.
        restart_with(rpm, road, gps);
        reason_ = Reason::Unsteady;
        return reason_;
    }
    reason_ = Reason::Steady;
    return reason_;
}

// --- Guide --------------------------------------------------------------

void Guide::open(uint8_t gear_count, const float* table) {
    count_ = gear_count > kMaxGears ? kMaxGears : gear_count;
    for (uint8_t i = 0; i < kMaxGears; ++i) table_[i] = i < count_ ? table[i] : 0.0f;
    phase_ = Phase::Intro;
    gear_ = 0;
    done_ = 0;
    circ_valid_ = false;
    circ_err_ = 0.0f;
    ready_ = false;
    reason_ = Reason::Steady;
    nearest_ = 0;
}

void Guide::cancel() {
    phase_ = Phase::Idle;
    done_ = 0;
    ready_ = false;
    circ_valid_ = false;
}

void Guide::enter_gear(uint8_t g) {
    gear_ = g;
    win_.reset();
    last_gen_ = win_.generation();
    ready_ = false;
    cand_ms_ = 0;
    cand_ratio_ = cand_gps_ = 0.0f;
    reason_ = Reason::Steady;
    nearest_ = 0;
    low_ = false;
}

void Guide::commit() {
    if (gear_ == 0 || gear_ > kMaxGears) return;
    done_ = static_cast<uint8_t>(done_ | (1u << (gear_ - 1)));
    ratio_[gear_ - 1] = cand_ratio_;
    gps_[gear_ - 1] = cand_gps_;
}

void Guide::advance() {
    uint8_t g = static_cast<uint8_t>(gear_ + 1);
    // A slot with no ratio is a gear the bike does not have.
    while (g <= count_ && table_[g - 1] <= 0.0f) ++g;
    if (g > count_) finish();
    else            enter_gear(g);
}

void Guide::finish() {
    phase_ = Phase::Summary;
    ready_ = false;
    float v[LearnConfig::kRing];
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxGears && n < LearnConfig::kRing; ++i) {
        if (done_ & (1u << i)) v[n++] = gps_[i];
    }
    const float m = median(v, n);
    circ_valid_ = n > 0 && abs_(m) >= cfg_.circ_report_frac;
    circ_err_ = circ_valid_ ? m : 0.0f;
}

bool Guide::next() {
    switch (phase_) {
        case Phase::Intro: {
            phase_ = Phase::Measuring;
            // Start before the first gear, then step onto it, so a table
            // with an empty first slot is handled by the same loop.
            gear_ = 0;
            advance();
            return false;
        }
        case Phase::Measuring:
            if (!ready_) return false;
            commit();
            advance();
            return false;
        case Phase::Summary:
            phase_ = Phase::Idle;
            return done_ != 0;
        case Phase::Idle:
            return false;
    }
    return false;
}

void Guide::skip() {
    if (phase_ == Phase::Measuring) advance();
}

float Guide::live_err() const {
    const float r = win_.ratio();
    if (r <= 0.0f || gear_ == 0 || table_[gear_ - 1] <= 0.0f) return 0.0f;
    return r / table_[gear_ - 1] - 1.0f;
}

bool Guide::has_result(uint8_t gear) const {
    return gear >= 1 && gear <= kMaxGears && (done_ & (1u << (gear - 1)));
}

float Guide::result_ratio(uint8_t gear) const {
    return has_result(gear) ? ratio_[gear - 1] : 0.0f;
}

float Guide::result_err(uint8_t gear) const {
    if (!has_result(gear) || table_[gear - 1] <= 0.0f) return 0.0f;
    return ratio_[gear - 1] / table_[gear - 1] - 1.0f;
}

void Guide::step(const Sample& s) {
    if (phase_ != Phase::Measuring) return;

    // A rider who has their ten seconds and then changes up is telling us
    // they are done with this gear. The ratio falls by about a gear step
    // and stays there; the run moves on without a button press, because
    // the button is on a handlebar pod they are riding with.
    if (ready_ && gear_ < count_) {
        const bool valid = !s.neutral && s.road_mph >= cfg_.steady.min_mph &&
                           s.rpm >= cfg_.steady.min_rpm;
        const float inst = valid ? s.rpm / s.road_mph : 0.0f;
        const bool lower = valid && inst < cand_ratio_ * (1.0f - cfg_.shift_frac) &&
                           inst > 0.4f * cand_ratio_;
        if (lower) {
            if (!low_) {
                low_ = true;
                low_since_ms_ = s.t_ms;
            } else if (s.t_ms - low_since_ms_ >= cfg_.shift_hold_ms) {
                commit();
                advance();
                return;
            }
        } else {
            low_ = false;
        }
    }

    reason_ = win_.feed(s, cfg_.steady);
    nearest_ = 0;
    if (reason_ != Reason::Steady) return;
    if (win_.duration_ms() < 2000) return;

    const float ratio = win_.ratio();
    const uint8_t near = nearest_gear(ratio, table_, count_);
    const float err = ratio / table_[gear_ - 1] - 1.0f;
    if (near != 0 && near != gear_ && abs_(err) > cfg_.wrong_gear_frac) {
        reason_ = Reason::WrongGear;
        nearest_ = near;
        return;
    }
    if (win_.duration_ms() >= cfg_.min_ms && win_.duration_ms() > cand_ms_) {
        cand_ms_ = win_.duration_ms();
        cand_ratio_ = ratio;
        cand_gps_ = win_.gps_over_road();
        ready_ = true;
    }
}

// --- Learner ------------------------------------------------------------

void Learner::reset() {
    for (uint8_t g = 0; g < kMaxGears; ++g) {
        n_[g] = head_[g] = 0;
        have_last_[g] = false;
        adjusted_[g] = false;
    }
    win_.reset();
    seen_gen_ = win_.generation();
}

void Learner::reset_gear(uint8_t gear) {
    if (gear < 1 || gear > kMaxGears) return;
    n_[gear - 1] = head_[gear - 1] = 0;
    have_last_[gear - 1] = false;
}

uint8_t Learner::episodes(uint8_t gear) const {
    return gear >= 1 && gear <= kMaxGears ? n_[gear - 1] : 0;
}

float Learner::error(uint8_t gear) const {
    if (gear < 1 || gear > kMaxGears) return 0.0f;
    return median(ring_[gear - 1], n_[gear - 1]);
}

bool Learner::adjusted(uint8_t gear) const {
    return gear >= 1 && gear <= kMaxGears && adjusted_[gear - 1];
}

uint8_t Learner::adjusted_mask() const {
    uint8_t m = 0;
    for (uint8_t i = 0; i < kMaxGears; ++i) if (adjusted_[i]) m = static_cast<uint8_t>(m | (1u << i));
    return m;
}

bool Learner::step(const Sample& s, const float* table, const float* base,
                   uint8_t count, uint8_t& gear, float& value) {
    const Reason r = win_.feed(s, cfg_.steady);
    if (r != Reason::Steady || win_.duration_ms() < cfg_.min_ms) return false;
    // One episode per stretch, however long it goes on.
    if (win_.generation() == seen_gen_) return false;
    seen_gen_ = win_.generation();

    const float ratio = win_.ratio();
    float err = 0.0f, second = 0.0f;
    const uint8_t g = nearest_gear(ratio, table, count, &err, &second);
    if (g == 0 || abs_(err) > cfg_.max_err || second < cfg_.neighbour_gap) return false;

    const uint8_t i = static_cast<uint8_t>(g - 1);
    if (have_last_[i] && s.t_ms - last_ms_[i] < cfg_.min_gap_ms) return false;
    have_last_[i] = true;
    last_ms_[i] = s.t_ms;

    ring_[i][head_[i]] = err;
    head_[i] = static_cast<uint8_t>((head_[i] + 1) % LearnConfig::kRing);
    if (n_[i] < LearnConfig::kRing) ++n_[i];

    if (adjusted_[i] || n_[i] < LearnConfig::kRing) return false;

    // Everything on record must lean the same way, and by a real amount.
    // The size of the move is the *smallest* error seen, so a handful of
    // slipping-clutch episodes (which only ever push the ratio up) cannot
    // drag the table further than the clean ones justify.
    float lo = ring_[i][0], hi = ring_[i][0], least = abs_(ring_[i][0]);
    for (uint8_t k = 1; k < LearnConfig::kRing; ++k) {
        const float e = ring_[i][k];
        if (e < lo) lo = e;
        if (e > hi) hi = e;
        if (abs_(e) < least) least = abs_(e);
    }
    if (!(lo > 0.0f || hi < 0.0f)) return false;          // mixed signs
    if (hi - lo > cfg_.max_spread) return false;
    if (least < cfg_.deadband) return false;

    float step = least > cfg_.max_step ? cfg_.max_step : least;
    if (hi < 0.0f) step = -step;
    float v = table[i] * (1.0f + step);
    // Round to a tenth, which is as fine as anyone will read it.
    v = static_cast<float>(static_cast<int32_t>(v * 10.0f + 0.5f)) / 10.0f;

    // Never further from the calibrated value than the drift limit. A
    // table the rider typed in by hand, far from where learning started,
    // is left alone rather than pulled back.
    const float b = base[i];
    if (b <= 0.0f || v > b * (1.0f + cfg_.max_drift) || v < b * (1.0f - cfg_.max_drift)) {
        return false;
    }

    adjusted_[i] = true;
    n_[i] = head_[i] = 0;
    gear = g;
    value = v;
    return true;
}

}  // namespace core::gearcal
