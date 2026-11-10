#include "core/trip/trip.hpp"

namespace core::trip {
namespace {
// A tick longer than this means the logic task was starved, or the counter
// wrapped. Charging a whole second of distance to one tick would put miles
// on the bike that nobody rode, so the tick is dropped instead.
constexpr uint32_t kMaxTickMs = 1000;
}  // namespace

float FuelModel::gph_at(float tps_pct) const {
    if (tps_pct < 0.0f)   tps_pct = 0.0f;
    if (tps_pct > 100.0f) tps_pct = 100.0f;
    return idle_gph + (wot_gph - idle_gph) * (tps_pct / 100.0f);
}

bool service_due(double engine_hours, float at_h, float every_h) {
    return every_h > 0.0f && engine_hours - static_cast<double>(at_h) >= static_cast<double>(every_h);
}

void Computer::restore(double odo_mi, double engine_hours) {
    odo_mi_ = odo_mi;
    engine_hours_ = engine_hours;
}

void Computer::update(const Input& in, const FuelModel& fm) {
    if (!started_) {
        last_ms_ = in.t_ms;
        started_ = true;
        return;                       // no elapsed time to account for yet
    }

    const uint32_t dt_ms = in.t_ms - last_ms_;    // unsigned: wrap-safe
    last_ms_ = in.t_ms;
    if (dt_ms == 0 || dt_ms > kMaxTickMs) return;

    const double dt_h = static_cast<double>(dt_ms) / 3600000.0;

    if (in.speed_mph > 0.0f) {
        const double miles = static_cast<double>(in.speed_mph) * dt_h;
        odo_mi_    += miles;
        trip_a_mi_ += miles;
        trip_b_mi_ += miles;
        if (in.speed_mph > top_mph_) top_mph_ = in.speed_mph;
    }

    if (in.engine_on) {
        engine_hours_  += dt_h;
        fuel_used_gal_ += static_cast<double>(fm.gph_at(in.tps_pct)) * dt_h;
    }
}

void Computer::reset_trip_a() { trip_a_mi_ = 0.0; top_mph_ = 0.0f; }
void Computer::reset_trip_b() { trip_b_mi_ = 0.0; }
void Computer::reset_fuel()   { fuel_used_gal_ = 0.0; }

state::Trip Computer::publish() const {
    state::Trip t;
    t.odo_mi        = static_cast<float>(odo_mi_);
    t.trip_a_mi     = static_cast<float>(trip_a_mi_);
    t.trip_b_mi     = static_cast<float>(trip_b_mi_);
    t.fuel_used_gal = static_cast<float>(fuel_used_gal_);
    t.eng_hours     = static_cast<float>(engine_hours_);
    t.top_mph       = top_mph_;
    return t;
}

}  // namespace core::trip
