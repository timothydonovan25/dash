#include "services/logic_tick.hpp"
#include "core/log/log.hpp"

namespace services {
namespace {

// How long a receiver may go without a new fix before the lap timer is told
// it has none: ten missed fixes at 10 Hz.
constexpr int32_t kFixSilentMs = 1000;

// Local feet from a reference fix, for the map traces. Fine over a track;
// this is a breadcrumb, not a survey.
void to_local_ft(const core::geo::LatLon& origin, const core::geo::LatLon& p,
                 float& x_ft, float& y_ft) {
    const double lat_ft_per_deg = 364000.0;
    const double lon_ft_per_deg = 364000.0 * 0.7660444;   // cos(40 deg)
    x_ft = static_cast<float>((p.lon - origin.lon) * lon_ft_per_deg);
    y_ft = static_cast<float>((p.lat - origin.lat) * lat_ft_per_deg);
}

uint8_t gear_count(const core::config::Settings& st) {
    return st.gear_count > core::gearcal::kMaxGears ? core::gearcal::kMaxGears
                                                     : st.gear_count;
}

// The table entries live at keys 0x0802..0x0807 (core/config.cpp).
constexpr uint16_t kGearKeyFirst = 0x0802;
constexpr uint16_t kGearKeyLast  = 0x0807;
// The same range the config table allows.
constexpr float kGearMin = 40.0f;
constexpr float kGearMax = 900.0f;

void publish_gear_cal(Deps& d, const LogicState& s) {
    using core::gearcal::kMaxGears;
    core::state::GearCal c;
    c.phase = static_cast<uint8_t>(s.guide.phase());
    c.gear = s.guide.gear();
    c.count = s.guide.phase() == core::gearcal::Phase::Idle ? gear_count(*d.settings)
                                                            : s.guide.count();
    c.reason = static_cast<uint8_t>(s.guide.reason());
    c.nearest = s.guide.nearest();
    c.ready = s.guide.ready();
    c.autotune = d.settings->gear_autotune;
    c.steady_ds = static_cast<uint16_t>(s.guide.steady_ms() / 100);
    c.need_ds = static_cast<uint16_t>(s.guide.need_ms() / 100);
    c.live_ratio = s.guide.live_ratio();
    c.live_err_pct = s.guide.live_err() * 100.0f;
    c.done_mask = s.guide.done_mask();
    c.circ_valid = s.guide.circ_valid();
    c.circ_err_pct = s.guide.circ_err() * 100.0f;
    for (uint8_t g = 1; g <= kMaxGears; ++g) {
        c.result_ratio[g - 1] = s.guide.result_ratio(g);
        c.result_err_pct[g - 1] = s.guide.result_err(g) * 100.0f;
        c.learn_n[g - 1] = s.learner.episodes(g);
        c.learn_err_pct[g - 1] = s.learner.error(g) * 100.0f;
    }
    c.adjusted_mask = s.learner.adjusted_mask();
    d.slots->gear_cal.publish(c);
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

    // The waypoints, which `keeper` saves while the bike is stopped.
    for (size_t i = 0; i < core::waypoint::kCount; ++i) {
        const uint16_t k = static_cast<uint16_t>(hal::kKeyWaypointBase + i * 3);
        double lat = 0.0, lon = 0.0, when = -1.0;
        if (hal::ok(d.storage->read_f64(hal::kNsLife, k, lat)) &&
            hal::ok(d.storage->read_f64(hal::kNsLife, static_cast<uint16_t>(k + 1), lon)) &&
            hal::ok(d.storage->read_f64(hal::kNsLife, static_cast<uint16_t>(k + 2), when))) {
            core::waypoint::decode(lat, lon, when, s.waypoints.pts[i]);
        }
    }
    if (d.slots) d.slots->waypoint.publish(s.waypoints);
}

void logic_tick(Deps& d, LogicState& s, ICommandSource& commands,
                uint32_t now, LogicPass pass) {
    const bool scheduled = pass == LogicPass::Scheduled;
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
                // The config screen has already edited Settings in place,
                // so for it this is a no-op; the Wi-Fi page has not, and
                // its value lands here. Then every module holding a copy
                // refreshes, and the keeper sees something to write to NVS.
                if (d.settings) {
                    if (const core::config::ConfigItem* it =
                            core::config::find(cmd.cfg.key)) {
                        if (it->kind != core::config::Kind::Action) {
                            core::config::set(*d.settings, *it, cmd.cfg.val);
                        }
                        // A ratio typed in by hand is the new calibrated
                        // value: it becomes the learner's base, and what
                        // the learner had seen of it is stale.
                        if (cmd.cfg.key >= kGearKeyFirst && cmd.cfg.key <= kGearKeyLast) {
                            const size_t gi = cmd.cfg.key - kGearKeyFirst;
                            d.settings->gear_base_rpm_per_mph[gi] =
                                d.settings->gear_rpm_per_mph[gi];
                            s.learner.reset_gear(static_cast<uint8_t>(gi + 1));
                        }
                    }
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
            case core::state::CommandId::BleForgetPhone:
                d.slots->ble_request.publish(core::state::BleRequest{
                    core::state::BleRequest::ForgetPhone});
                break;
            case core::state::CommandId::BlePairingMode:
                d.slots->ble_request.publish(core::state::BleRequest{
                    core::state::BleRequest::PairingMode});
                break;
            case core::state::CommandId::WifiToggle:
                d.slots->wifi_request.publish(core::state::WifiRequest{
                    core::state::WifiRequest::Toggle});
                break;
            case core::state::CommandId::NavDropWaypoint:
                // Like the finish line: this task decides whether the fix
                // is good enough to drop one on.
                if (g.fix && cmd.i >= 0 &&
                    static_cast<size_t>(cmd.i) < core::waypoint::kCount) {
                    s.waypoints.pts[cmd.i] = core::state::Waypoint{
                        true, g.lat, g.lon, g.utc_ms,
                        core::waypoint::gps_day(g.year, g.day_of_year)};
                    d.slots->waypoint.publish(s.waypoints);
                }
                break;
            case core::state::CommandId::NavClearWaypoint:
                if (cmd.i >= 0 && static_cast<size_t>(cmd.i) < core::waypoint::kCount) {
                    s.waypoints.pts[cmd.i] = core::state::Waypoint{};
                    d.slots->waypoint.publish(s.waypoints);
                }
                break;
            case core::state::CommandId::StorageEject:
                d.slots->storage_request.publish(core::state::StorageRequest{
                    core::state::StorageRequest::Eject});
                break;
            case core::state::CommandId::OtaInstallSd:
                d.slots->ota_request.publish(core::state::OtaRequest{
                    core::state::OtaRequest::InstallSd});
                break;
            case core::state::CommandId::OtaBootDevOnce:
                d.slots->ota_request.publish(core::state::OtaRequest{
                    core::state::OtaRequest::BootDevOnce});
                break;
            case core::state::CommandId::OtaPromote:
                d.slots->ota_request.publish(core::state::OtaRequest{
                    core::state::OtaRequest::Promote});
                break;
            case core::state::CommandId::GearCalStart:
                if (d.settings && s.guide.phase() == core::gearcal::Phase::Idle) {
                    s.guide.open(gear_count(*d.settings), d.settings->gear_rpm_per_mph);
                }
                break;
            case core::state::CommandId::GearCalNext:
                // Accepting a run writes it into the table, and into the
                // base, which is what "calibrated" means to the learner.
                if (d.settings && s.guide.next()) {
                    for (uint8_t gi = 1; gi <= core::gearcal::kMaxGears; ++gi) {
                        if (!s.guide.has_result(gi)) continue;
                        float v = static_cast<float>(static_cast<int32_t>(
                                      s.guide.result_ratio(gi) * 10.0f + 0.5f)) / 10.0f;
                        v = v < kGearMin ? kGearMin : (v > kGearMax ? kGearMax : v);
                        d.settings->gear_rpm_per_mph[gi - 1] = v;
                        d.settings->gear_base_rpm_per_mph[gi - 1] = v;
                    }
                    s.learner.reset();
                }
                break;
            case core::state::CommandId::GearCalSkip:
                s.guide.skip();
                break;
            case core::state::CommandId::GearCalCancel:
                s.guide.cancel();
                break;
            case core::state::CommandId::GearCalReset:
                if (d.settings) {
                    for (size_t gi = 0; gi < core::gearcal::kMaxGears; ++gi) {
                        d.settings->gear_rpm_per_mph[gi] = d.settings->gear_base_rpm_per_mph[gi];
                    }
                    s.learner.reset();
                }
                break;
            default:
                break;
        }
    }

    // --- fused speed ----------------------------------------------------
    // Fed from the slots like everything else here. A new fix is one whose
    // UTC moved: Doppler must be believed once per fix, not once per tick.
    const core::state::Attitude att = d.slots->attitude.get();
    const core::state::Health health = d.slots->health.get();
    core::fusion::SpeedInput fi;
    fi.t_ms = now;
    fi.accel_g = att.g_lon;
    fi.accel_ok = health.ok(core::state::Dev::Imu);
    // The wheels have no health of their own: a dead sensor reads 0, and a
    // 0 that disagrees with GPS and the IMU is gated out like a spin.
    fi.front_ok = fi.rear_ok = true;
    fi.front_mph = m.front_mph;
    fi.rear_mph = m.rear_mph;
    fi.gps_ok = g.fix;
    fi.gps_mph = g.speed_mph;
    fi.hdop = g.hdop;
    fi.gps_new = g.fix && g.utc_ms != s.last_gps_utc_ms;
    s.last_gps_utc_ms = g.utc_ms;
    const core::fusion::SpeedOutput fo = s.speed.step(fi);
    core::state::Fused fused;
    fused.speed_mph = fo.mph;
    fused.speed_sigma_mph = fo.sigma_mph;
    fused.speed_valid = fo.valid;
    fused.used = fo.used;
    fused.suspect = fo.suspect;
    fused.resets = fo.resets;
    fused.accel_bias_g = fo.bias_g;
    d.slots->fused.publish(fused);

    // Dead wheel sensors (core::fusion::WheelCheck) into the health table,
    // where DIAG -> Sensors, the cruise fault line and `ctrl`'s choice of
    // speedo source all see them. Unknown stays Absent: a wheel nobody has
    // cross-checked yet is not a fault, and is not known good either.
    s.wheels.update(fi, fo);
    if (d.health) {
        const auto report = [&](core::state::Dev dev, core::fusion::Wheel w) {
            const core::state::DevState st = (*d.health)[dev].state;
            // Absent until first judged. The tracker ignores reports for an
            // absent device, so the first verdict either way brings it in.
            if (w != core::fusion::Wheel::Unknown && d.health->is_absent(dev)) {
                d.health->begin_init(dev, now);
            }
            if (w == core::fusion::Wheel::Ok) {
                d.health->report_ok(dev, now);
            } else if (w == core::fusion::Wheel::Dead && st != core::state::DevState::Fault) {
                // Errors until the tracker calls it Fault, then quiet: a
                // Fault is not aged, and fault_count should count events,
                // not ticks. (`st` is the keeper's last verdict, up to one
                // 50 ms keeper tick behind.)
                d.health->report_error(dev, 0, now);
            }
        };
        report(core::state::Dev::FrontWheel, s.wheels.front());
        report(core::state::Dev::RearWheel, s.wheels.rear());
    }

    // --- gear calibration -----------------------------------------------
    // The guided run while it is open; otherwise the learner, if it is on.
    // The learner's window resumes from nothing after a run (the gap in its
    // samples throws the old one away), so the two never share evidence.
    // Scheduled passes only: its windows are means over samples, and an
    // extra sample at every fix would weight those moments over the rest.
    if (d.settings && scheduled) {
        core::gearcal::Sample gs;
        gs.t_ms = now;
        gs.rpm = e.rpm;
        gs.road_mph = m.best_mph;
        gs.gps_mph = g.speed_mph;
        gs.gps_ok = g.fix;
        gs.hdop = g.hdop;
        gs.neutral = e.neutral;
        if (s.guide.phase() != core::gearcal::Phase::Idle) {
            s.guide.step(gs);
        } else if (d.settings->gear_autotune) {
            uint8_t gear = 0;
            float value = 0.0f;
            if (s.learner.step(gs, d.settings->gear_rpm_per_mph,
                               d.settings->gear_base_rpm_per_mph,
                               gear_count(*d.settings), gear, value)) {
                d.settings->gear_rpm_per_mph[gear - 1] = value;
            }
        }
        publish_gear_cal(d, s);
    }

    // --- START ----------------------------------------------------------
    // Once a power-on, at the first good fix with a date: the way back to
    // where this ride began. A START from earlier in the same ride stays
    // (core::waypoint::start_is_stale), so a key-off at lunch keeps home.
    if (!s.start_checked && core::waypoint::good_fix(g) && g.year != 0) {
        s.start_checked = true;
        const uint32_t day = core::waypoint::gps_day(g.year, g.day_of_year);
        core::state::Waypoint& start = s.waypoints.pts[core::waypoint::Start];
        if (core::waypoint::start_is_stale(start, day, g.utc_ms)) {
            start = core::state::Waypoint{true, g.lat, g.lon, g.utc_ms, day};
            d.slots->waypoint.publish(s.waypoints);
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

    // Once per new fix, at the time it was taken. The gnss task wakes this
    // pass when a fix lands, and the 50 Hz ticks see the same fix again
    // between them: handing the timer the same fix five times made every
    // segment look 20 ms long, so each line crossing was interpolated
    // over a fifth of the real interval and landed up to ~80 ms off.
    //
    // A receiver that stops talking leaves `fix` set on its last sentence;
    // a second of silence is told to the timer as no fix, so a lap across
    // the dropout is marked rather than scored as if nothing happened.
    const bool new_fix = g.rx_ms != s.last_lap_rx_ms;
    const bool silent = static_cast<int32_t>(now - g.rx_ms) > kFixSilentMs;
    core::lap::Fix lf;
    lf.pos = core::geo::LatLon{g.lat, g.lon};
    lf.speed_mph = g.speed_mph;      // Doppler, not differenced positions
    core::lap::Update lu;
    if (new_fix) {
        s.last_lap_rx_ms = g.rx_ms;
        s.lap_told_stale = false;
        // The fix's own GPS time on the system clock, not its arrival: the
        // arrival carries the receiver's varying latency, the UART and the
        // poll, and a lap time is the difference of two of them. Without a
        // fix the time of day may not be there yet, so arrival it is.
        s.last_fix_ms = g.fix ? s.fix_clock.stamp(g.utc_ms, g.rx_ms) : g.rx_ms;
        lf.valid = g.fix;
        lf.t_ms = s.last_fix_ms;
        lu = s.lap.update(lf, lc);
    } else if (silent && !s.lap_told_stale && s.last_lap_rx_ms != 0) {
        s.lap_told_stale = true;
        lf.valid = false;
        lf.t_ms = now;
        lu = s.lap.update(lf, lc);
    }
    s.lap.advance(now);
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
    // One point per fix, at most 10 Hz (the sim makes a fix every tick).
    // Spaced by the fixes' own times, not by this tick's: fixes 100 ms
    // apart land on a 20 ms tick grid, so two can reach here 80 ms apart,
    // and a gate on `now` dropped the second one -- a 200 ms hole in the
    // trace and the delta, for minutes at a time as the two clocks drifted.
    if (g.fix && new_fix && g.rx_ms - s.last_trace_ms >= 90) {
        s.last_trace_ms = g.rx_ms;
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
            // Lap time at the fix, not at this pass: the position is the
            // fix's, and the pass runs a little after it (the gnss poll, or
            // a whole tick if the fix arrived with no wake).
            const uint32_t cur = s.lap.state().current_ms;
            const uint32_t age = now - s.last_fix_ms;
            d.delta->sample(p.x_ft, p.y_ft, cur > age ? cur - age : 0, s.delta_cfg);
        }
    }

    // --- blackbox -------------------------------------------------------
    // Filled here rather than by each producer so one record is one coherent
    // moment. push() never blocks and never waits on the card; a full ring
    // drops the record and counts it.
    //
    // Not every tick: the rate is a setting (§13), and the decimator decides.
    // A lap event forces a row through whatever the rate, because the marker
    // rides in the record and this is the only pass that carries it. Other
    // than that, Scheduled passes only, so the rows keep the tick's cadence
    // however many fixes and commands woke `logic` in between.
    const uint8_t log_rate = d.settings
        ? d.settings->log_rate
        : static_cast<uint8_t>(core::log::kDefaultRate);
    if (d.blackbox && (scheduled || lap_event) &&
        s.log_gate.due(log_rate, now, lap_event)) {
        const core::state::Attitude& a = att;
        core::log::Record rec;
        rec.front_mph = m.front_mph;
        rec.rear_mph = m.rear_mph;
        rec.gps_mph = g.speed_mph;
        rec.hdop = g.hdop;
        rec.gps_utc_ms = g.utc_ms;
        rec.fused_mph = fo.mph;
        rec.fused_sigma = fo.sigma_mph;
        rec.alt_ft = g.alt_m * 3.28084f;
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
