#include "services/logic_tick.hpp"
#include "core/log/log.hpp"

namespace services {
namespace {

// Local feet from a reference fix, for the map traces. Fine over a track;
// this is a breadcrumb, not a survey.
void to_local_ft(const core::geo::LatLon& origin, const core::geo::LatLon& p,
                 float& x_ft, float& y_ft) {
    const double lat_ft_per_deg = 364000.0;
    const double lon_ft_per_deg = 364000.0 * 0.7660444;   // cos(40 deg)
    x_ft = static_cast<float>((p.lon - origin.lon) * lon_ft_per_deg);
    y_ft = static_cast<float>((p.lat - origin.lat) * lat_ft_per_deg);
}

}  // namespace

void logic_restore(Deps& d, LogicState& s) {
    // The odometer and engine hours come back from NVS. Everything else
    // starts at zero, because everything else is a trip, not a life.
    if (!d.storage) return;
    double odo = 0.0, hours = 0.0;
    d.storage->read_f64(hal::kNsLife, hal::kKeyOdometerMi, odo);
    d.storage->read_f64(hal::kNsLife, hal::kKeyEngineHours, hours);
    s.trip.restore(odo, hours);
}

void logic_tick(Deps& d, LogicState& s, ICommandSource& commands,
                uint32_t now) {
    const core::state::Gnss   g = d.slots->gnss.get();
    const core::state::Motion m = d.slots->motion.get();
    const core::state::Engine e = d.slots->engine.get();

    // --- commands -------------------------------------------------------
    core::state::Command cmd;
    while (commands.take(cmd)) {
        switch (cmd.id) {
            case core::state::CommandId::LapSetLine:
                // The screen asked; this task decides whether the fix is
                // good enough, because the screen only has a Frame.
                if (g.fix) {
                    s.lap.set_line(core::geo::LatLon{g.lat, g.lon}, g.course_deg);
                }
                break;
            case core::state::CommandId::LapArm:
                s.lap.arm();
                break;
            case core::state::CommandId::LapClear:
                s.lap.clear();
                s.lap_count = 0;
                if (d.lap_history) *d.lap_history = LapHistory{};
                if (d.lap_trace) d.lap_trace->clear();
                if (d.delta) d.delta->clear();
                break;
            case core::state::CommandId::TripReset:
                if (cmd.i == 1) s.trip.reset_trip_b();
                else            s.trip.reset_trip_a();
                break;
            case core::state::CommandId::ConfigSet:
                // Settings are edited in place by the config screen. The
                // command exists so every module holding a copy refreshes,
                // and so the keeper knows something needs writing to NVS.
                if (d.settings) {
                    s.fuel.idle_gph = d.settings->fuel_idle_gph;
                    s.fuel.wot_gph  = d.settings->fuel_wot_gph;
                    s.fuel.tank_gal = d.settings->fuel_tank_gal;
                }
                // A new logging rate starts its cadence now rather than from
                // whenever the last row happened to land.
                s.log_gate.reset();
                break;
            case core::state::CommandId::MediaCtrl:
                // Just a hand-off: whichever task owns the radio drains
                // this on its own schedule via Published<T>::generation().
                d.slots->media_ctrl.publish(
                    core::state::MediaCtrl{static_cast<uint8_t>(cmd.i)});
                break;
            default:
                break;
        }
    }

    // --- trip -----------------------------------------------------------
    core::trip::Input ti;
    ti.speed_mph = m.best_mph;
    ti.engine_on = e.rpm > 0.0f;
    ti.tps_pct = e.tps_pct;
    ti.t_ms = now;
    s.trip.update(ti, s.fuel);
    d.slots->trip.publish(s.trip.publish());

    // --- lap ------------------------------------------------------------
    core::lap::Config lc;
    if (d.settings) {
        const core::config::LapView lv = core::config::lap_view(*d.settings);
        lc.half_width_ft = lv.half_width_ft;
        lc.min_speed_mph = lv.min_speed_mph;
        lc.min_lap_ms = lv.min_lap_ms;
    }

    // Picked up by the blackbox row below, so a lap tag lands between the
    // right two data rows rather than at the end of the file.
    uint32_t lap_done_ms = 0;
    uint16_t lap_done_index = 0;

    core::lap::Fix lf;
    lf.pos = core::geo::LatLon{g.lat, g.lon};
    lf.speed_mph = g.speed_mph;      // Doppler, not differenced positions
    lf.valid = g.fix;
    lf.t_ms = now;

    const core::lap::Update lu = s.lap.update(lf, lc);
    const bool lap_event = lu.event != core::lap::Event::None;
    if (lu.event == core::lap::Event::Started) {
        if (d.delta) d.delta->start_lap();
    } else if (lu.event == core::lap::Event::Completed) {
        // The lap tag in the log is 0-based, so it is the count *before*
        // this lap is added.
        lap_done_index = s.lap_count;
        lap_done_ms = lu.lap_ms;
        if (s.lap_count < LogicState::kMaxLaps) s.laps_ms[s.lap_count++] = lu.lap_ms;
        if (d.lap_history && d.lap_history->count < LapHistory::kMax) {
            d.lap_history->ms[d.lap_history->count++] = lu.lap_ms;
            d.lap_history->best_ms = s.lap.state().best_ms;
        }
        if (d.lap_trace) d.lap_trace->clear();      // trace the new lap
        if (d.delta) {
            // The reference is the session best. The lap timer has already
            // refused to let a lap with a fix gap in it set the best, so a
            // dropout cannot poison the reference either.
            const bool is_best = lu.valid && lu.lap_ms == s.lap.state().best_ms;
            d.delta->finish_lap(is_best);
            d.delta->start_lap();
        }
    }

    int32_t delta_ms = 0;
    bool delta_valid = false;
    if (d.delta) delta_valid = d.delta->delta_ms(delta_ms);
    d.slots->lap.publish(
        core::state::to_published(s.lap.state(), delta_ms, delta_valid));

    // --- traces ---------------------------------------------------------
    if (g.fix && now - s.last_trace_ms >= 100) {     // 10 Hz, one per fix
        s.last_trace_ms = now;
        if (!s.have_origin) {
            s.origin = core::geo::LatLon{g.lat, g.lon};
            s.have_origin = true;
        }
        core::state::TrackPoint p;
        to_local_ft(s.origin, core::geo::LatLon{g.lat, g.lon}, p.x_ft, p.y_ft);
        p.speed_mph = m.best_mph;
        p.t_ms = now;
        if (d.trip_trace) d.trip_trace->push(p);
        if (d.lap_trace) d.lap_trace->push(p);
        if (d.delta && s.lap.state().phase == core::lap::Phase::Timing) {
            d.delta->sample(p.x_ft, p.y_ft, s.lap.state().current_ms, s.delta_cfg);
        }
    }

    // --- blackbox -------------------------------------------------------
    // Filled here rather than by each producer so one record is one coherent
    // moment. push() never blocks and never waits on the card; a full ring
    // drops the record and counts it.
    //
    // Not every tick: the rate is a setting (§13), and the decimator decides.
    // A lap event forces a row through whatever the rate, because the marker
    // rides in the record and this is the only tick that carries it.
    const uint8_t log_rate = d.settings
        ? d.settings->log_rate
        : static_cast<uint8_t>(core::log::kDefaultRate);
    if (d.blackbox && s.log_gate.due(log_rate, now, lap_event)) {
        const core::state::Attitude a = d.slots->attitude.get();
        core::log::Record rec;
        rec.t_ms = now;
        rec.speed_mph = m.best_mph;
        rec.rpm = e.rpm;
        rec.lean_deg = a.lean_deg;
        rec.pitch_deg = a.pitch_deg;
        rec.g_lat = a.g_lat;
        rec.g_lon = a.g_lon;
        rec.g_vert = a.g_vert;
        rec.course_deg = g.course_deg;
        rec.lat = g.lat;
        rec.lon = g.lon;
        rec.lap_ms = s.lap.state().current_ms;
        rec.lap_done_ms = lap_done_ms;
        rec.lap_num = lap_done_ms != 0 ? lap_done_index
                                       : static_cast<uint16_t>(s.lap_count);
        // RaceRender has no "unknown" gear, so neutral and no-idea share 0.
        // kFlagNeutral below is the sensed one; this is the inference.
        rec.gear = e.neutral ? 0 : e.gear;
        rec.sats = g.sats;
        rec.flags = static_cast<uint16_t>(
            (e.neutral ? core::log::kFlagNeutral : 0) |
            (a.airborne ? core::log::kFlagAirborne : 0) |
            (g.fix ? core::log::kFlagFix : 0) |
            (s.lap.state().phase == core::lap::Phase::Timing
                 ? core::log::kFlagTiming : 0) |
            (e.ecu_online ? core::log::kFlagEcu : 0));
        d.blackbox->push(rec);
    }
}

}  // namespace services
