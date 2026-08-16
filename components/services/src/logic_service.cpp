// `logic` — core 0, priority 10, 50 Hz.
//
// The lap timer, the trip computer, the breadcrumb traces, and the one place
// commands are drained. Every decision it makes lives in a core module with
// host tests behind it; this file decides when to call them and what to do
// with the answer.
#include "services/services.hpp"
#include "core/lap/lap.hpp"
#include "core/trip/trip.hpp"
#include "core/geo/geo.hpp"
#include "core/log/log.hpp"
#include "core/delta/delta.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <utility>

namespace services {
namespace {

// Lap history for the Track history screen. Fixed size: a session of more
// than this many laps on a CRF250F would need more fuel than it carries.
constexpr size_t kMaxLaps = 64;

struct Session {
    uint32_t laps_ms[kMaxLaps] = {};
    size_t   count = 0;
    void add(uint32_t ms) {
        if (count < kMaxLaps) laps_ms[count++] = ms;
    }
};

// Local feet from a reference fix, for the map traces. Fine over a track;
// this is a breadcrumb, not a survey.
void to_local_ft(const core::geo::LatLon& origin, const core::geo::LatLon& p,
                 float& x_ft, float& y_ft) {
    const double lat_ft_per_deg = 364000.0;
    const double lon_ft_per_deg = 364000.0 * 0.7660444;   // cos(40 deg)
    x_ft = static_cast<float>((p.lon - origin.lon) * lon_ft_per_deg);
    y_ft = static_cast<float>((p.lat - origin.lat) * lat_ft_per_deg);
}

void logic_task(void* arg) {
    auto* pair = static_cast<std::pair<Deps*, CommandQueue*>*>(arg);
    Deps& d = *pair->first;
    CommandQueue& q = *pair->second;

    core::lap::Timer     lap;
    core::trip::Computer trip;
    core::trip::FuelModel fuel;
    core::delta::Config   dcfg;
    Session session;

    core::geo::LatLon origin;
    bool have_origin = false;
    uint32_t last_trace_ms = 0;

    // The odometer and engine hours come back from NVS. Everything else
    // starts at zero, because everything else is a trip, not a life.
    if (d.storage) {
        double odo = 0.0, hours = 0.0;
        d.storage->read_f64(hal::kNsLife, hal::kKeyOdometerMi, odo);
        d.storage->read_f64(hal::kNsLife, hal::kKeyEngineHours, hours);
        trip.restore(odo, hours);
    }

    for (;;) {
        const uint32_t now = d.clock->now_ms();
        const core::state::Gnss g = d.slots->gnss.get();
        const core::state::Motion m = d.slots->motion.get();
        const core::state::Engine e = d.slots->engine.get();

        // --- commands ---------------------------------------------------
        core::state::Command cmd;
        while (q.take(cmd)) {
            switch (cmd.id) {
                case core::state::CommandId::LapSetLine:
                    // The screen asked; this task decides whether the fix is
                    // good enough, because the screen only has a Frame.
                    if (g.fix) {
                        lap.set_line(core::geo::LatLon{g.lat, g.lon},
                                     g.course_deg);
                    }
                    break;
                case core::state::CommandId::LapArm:   lap.arm(); break;
                case core::state::CommandId::LapClear:
                    lap.clear();
                    session.count = 0;
                    if (d.lap_trace) d.lap_trace->clear();
                    if (d.delta) d.delta->clear();
                    break;
                case core::state::CommandId::TripReset:
                    if (cmd.i == 1) trip.reset_trip_b();
                    else            trip.reset_trip_a();
                    break;
                case core::state::CommandId::ConfigSet:
                    // Settings are edited in place by the config screen; the
                    // command exists so the keeper knows to write NVS and so
                    // every command is logged in one place.
                    fuel.idle_gph = d.settings->fuel_idle_gph;
                    fuel.wot_gph  = d.settings->fuel_wot_gph;
                    fuel.tank_gal = d.settings->fuel_tank_gal;
                    break;
                default:
                    break;
            }
        }

        // --- trip -------------------------------------------------------
        core::trip::Input ti;
        ti.speed_mph = m.best_mph;
        ti.engine_on = e.rpm > 0.0f;
        ti.tps_pct = e.tps_pct;
        ti.t_ms = now;
        trip.update(ti, fuel);
        d.slots->trip.publish(trip.publish());

        // --- lap --------------------------------------------------------
        const core::config::LapView lv = core::config::lap_view(*d.settings);
        core::lap::Config lc;
        lc.half_width_ft = lv.half_width_ft;
        lc.min_speed_mph = lv.min_speed_mph;
        lc.min_lap_ms = lv.min_lap_ms;

        core::lap::Fix lf;
        lf.pos = core::geo::LatLon{g.lat, g.lon};
        lf.speed_mph = g.speed_mph;      // Doppler, not differenced positions
        lf.valid = g.fix;
        lf.t_ms = now;

        const core::lap::Update lu = lap.update(lf, lc);
        if (lu.event == core::lap::Event::Started) {
            if (d.delta) d.delta->start_lap();
        } else if (lu.event == core::lap::Event::Completed) {
            session.add(lu.lap_ms);
            if (d.lap_trace) d.lap_trace->clear();   // trace the new lap
            if (d.delta) {
                // The reference is the session best. The lap timer has
                // already refused to let a lap with a fix gap in it set the
                // best, so a dropout cannot poison the reference either.
                const bool is_best = lu.valid && lu.lap_ms == lap.state().best_ms;
                d.delta->finish_lap(is_best);
                d.delta->start_lap();
            }
        }

        int32_t delta_ms = 0;
        bool delta_valid = false;
        if (d.delta) delta_valid = d.delta->delta_ms(delta_ms);
        d.slots->lap.publish(
            core::state::to_published(lap.state(), delta_ms, delta_valid));

        // --- traces -----------------------------------------------------
        if (g.fix && now - last_trace_ms >= 100) {       // 10 Hz, one per fix
            last_trace_ms = now;
            if (!have_origin) {
                origin = core::geo::LatLon{g.lat, g.lon};
                have_origin = true;
            }
            core::state::TrackPoint p;
            to_local_ft(origin, core::geo::LatLon{g.lat, g.lon}, p.x_ft, p.y_ft);
            p.speed_mph = m.best_mph;
            p.t_ms = now;
            if (d.trip_trace) d.trip_trace->push(p);
            if (d.lap_trace) d.lap_trace->push(p);
            // The delta matcher wants the same points, but timed from the
            // crossing rather than from boot.
            if (d.delta && lap.state().phase == core::lap::Phase::Timing) {
                d.delta->sample(p.x_ft, p.y_ft, lap.state().current_ms, dcfg);
            }
        }

        // --- blackbox ---------------------------------------------------
        // Pushed here rather than by each producer so one record is one
        // coherent moment. push() never blocks and never waits on the card;
        // a full ring drops the record and counts it.
        if (d.blackbox) {
            const core::state::Attitude a = d.slots->attitude.get();
            core::log::Record rec;
            rec.t_ms = now;
            rec.speed_mph = m.best_mph;
            rec.rpm = e.rpm;
            rec.lean_deg = a.lean_deg;
            rec.pitch_deg = a.pitch_deg;
            rec.g_lat = a.g_lat;
            rec.g_vert = a.g_vert;
            rec.lat = g.lat;
            rec.lon = g.lon;
            rec.lap_ms = lap.state().current_ms;
            rec.sats = g.sats;
            rec.flags = static_cast<uint16_t>(
                (e.neutral ? core::log::kFlagNeutral : 0) |
                (a.airborne ? core::log::kFlagAirborne : 0) |
                (g.fix ? core::log::kFlagFix : 0) |
                (lap.state().phase == core::lap::Phase::Timing
                     ? core::log::kFlagTiming : 0) |
                (e.ecu_online ? core::log::kFlagEcu : 0));
            d.blackbox->push(rec);
        }

        vTaskDelay(pdMS_TO_TICKS(20));      // 50 Hz
    }
}

// Not shared state: xTaskCreate takes one void*, and this is where the two
// references live long enough to survive start_logic() returning. Written
// once, before the task exists, and read only by it.
std::pair<Deps*, CommandQueue*> g_logic_arg;

}  // namespace

void start_logic(Deps& d, CommandQueue& q) {
    g_logic_arg = {&d, &q};
    xTaskCreatePinnedToCore(logic_task, "logic", 6144, &g_logic_arg, 10,
                            nullptr, 0);
}

}  // namespace services
