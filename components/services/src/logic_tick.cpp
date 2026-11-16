#include "services/logic_tick.hpp"
#include "core/units/units.hpp"
#include "core/hr/hr.hpp"
#include "core/log/log.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace services {
namespace {

// Local feet from a reference fix, for the map traces, the lap delta and
// saved references. Fine over a track; this is a breadcrumb, not a survey.
// A degree of longitude is cos(latitude) of a degree of latitude, at the
// origin's latitude: it was cos(40°) wherever the bike was, which stretched
// east-west by 12% at 47° and squashed it by 11% at 33°, and the delta
// matcher's distances with it. Still linear, so a saved reference moves
// between frames by where its line sits (core::track::shift); a frame
// started a mile away at the same track changes the scale by 1e-4.
void to_local_ft(const core::geo::LatLon& origin, const core::geo::LatLon& p,
                 float& x_ft, float& y_ft) {
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double lat_ft_per_deg = 364000.0;
    const double lon_ft_per_deg = 364000.0 * std::cos(origin.lat * kDeg2Rad);
    x_ft = static_cast<float>((p.lon - origin.lon) * lon_ft_per_deg);
    y_ft = static_cast<float>((p.lat - origin.lat) * lat_ft_per_deg);
}

void ensure_origin(LogicState& s, const core::geo::LatLon& p) {
    if (!s.have_origin) {
        s.origin = p;
        s.have_origin = true;
    }
}

uint32_t gps_today(const core::state::Gnss& g) {
    return g.year != 0 ? core::waypoint::gps_day(g.year, g.day_of_year) : 0;
}

// --- saved tracks (core::track) ------------------------------------------

// The track's finish line in this power-on's local frame: what a saved
// reference, kept relative to the line, is moved by.
void line_in_frame(const LogicState& s, const core::track::Record& r, float& x, float& y) {
    to_local_ft(s.origin, r.finish.at, x, y);
}

// A line has been set, or has come back: whatever the session knew about
// the old one (the splits, the reference) was about the old one.
void forget_line_state(Deps& d, LogicState& s) {
    s.sectors.reset();
    s.last_split = core::lap::Split{};
    if (d.delta) d.delta->clear();
    s.ref_ms = 0;
    s.want_ref = false;
}

// The rider set a line: a saved track's line moved (its old record goes),
// or a new track. Before the library has come off the card there is no
// telling which, so that waits for it, holding where the line is.
void adopt_new_line(Deps& d, LogicState& s, const core::track::Line& line, uint32_t today) {
    s.track = -1;
    s.track_recalled = false;
    s.track_unsaved = false;
    if (!d.tracks) return;
    if (!s.tracks_known) {
        s.track_unsaved = true;
        s.pending_line = line;
        s.pending_day = today;
        return;
    }
    const int slot = core::track::slot_for_line(d.tracks->lib, line.at);
    if (slot < 0) return;
    d.tracks->lib.rec[slot] = core::track::fresh(line, today);
    s.track = static_cast<int8_t>(slot);
    s.track_dirty = true;
}

// A good fix near a saved track, with no line set: the line, the splits and
// (once the card has answered) the reference come back.
void recall_track(Deps& d, LogicState& s, int slot, const core::state::Gnss& g) {
    core::track::Record& r = d.tracks->lib.rec[slot];
    s.lap.set_line(r.finish.at, r.finish.heading_deg);
    forget_line_state(d, s);
    if (r.splits) {
        core::geo::LatLon at[core::lap::kSectors - 1];
        double hd[core::lap::kSectors - 1];
        for (int k = 0; k < core::lap::kSectors - 1; ++k) {
            at[k] = r.split[k].at;
            hd[k] = r.split[k].heading_deg;
        }
        s.sectors.place_lines(at, hd);
    }
    ensure_origin(s, core::geo::LatLon{g.lat, g.lon});
    s.track = static_cast<int8_t>(slot);
    s.track_recalled = true;
    s.want_ref = r.ref_ms != 0;
    s.load_tries = 0;
    const uint32_t today = gps_today(g);
    if (today != 0) r.last_day = today;
    if (r.sessions != UINT16_MAX) ++r.sessions;
    s.track_dirty = true;
}

// One turn of the hand-off with `storage`: take an answer if there is one,
// then post the next job if the box is free. Never waits.
void pump_tracks(Deps& d, LogicState& s, uint32_t now) {
    TrackMailbox* box = d.tracks;
    if (!box) return;
    using Op = TrackMailbox::Op;

    if (box->answered()) {
        switch (box->op) {
            case Op::List:
                // A card that could not be read is asked again in a while;
                // an empty library from it would hand its slots out anew.
                if (box->ok) {
                    s.tracks_known = true;
                } else {
                    s.tracks_asked = false;
                    s.retry_at_ms = (now + 5000) | 1u;
                }
                break;
            case Op::Load:
                // The card could not be read. A save after giving up writes
                // the track without its reference, so a card that was only
                // busy gets two more goes before that happens.
                if (!box->ok && ++s.load_tries < 3) {
                    s.retry_at_ms = (now + 5000) | 1u;
                    break;
                }
                s.want_ref = false;
                if (box->ok && s.track >= 0 && box->slot == static_cast<uint8_t>(s.track) &&
                    box->n >= 2 && box->rec.ref_ms != 0 && d.delta && s.have_origin) {
                    float x = 0.0f, y = 0.0f;
                    line_in_frame(s, d.tracks->lib.rec[s.track], x, y);
                    core::track::shift(box->pts, box->n, x, y);
                    d.delta->load_reference(box->pts, box->n);
                    s.ref_ms = box->rec.ref_ms;
                }
                break;
            case Op::Save:
                // A card that will not take it gets another go in a while,
                // not every pass (rule 5).
                if (!box->ok) {
                    s.track_dirty = true;
                    s.retry_at_ms = (now + 5000) | 1u;
                }
                break;
            case Op::Forget:
            case Op::None:
                break;
        }
        box->release();
    }
    if (!box->free()) return;
    if (s.retry_at_ms != 0) {
        if (static_cast<int32_t>(now - s.retry_at_ms) < 0) return;
        s.retry_at_ms = 0;
    }

    if (!s.tracks_asked) {
        s.tracks_asked = true;
        box->op = Op::List;
        box->post();
        return;
    }
    if (!s.tracks_known) return;

    if (s.forget_slot >= 0) {
        box->op = Op::Forget;
        box->slot = static_cast<uint8_t>(s.forget_slot);
        s.forget_slot = -1;
        box->post();
        return;
    }
    if (s.track < 0) return;
    if (s.want_ref) {
        box->op = Op::Load;
        box->slot = static_cast<uint8_t>(s.track);
        box->post();
        return;
    }
    if (s.track_dirty) {
        core::track::Record& r = box->lib.rec[s.track];
        box->n = 0;
        r.ref_ms = 0;
        if (s.ref_ms != 0 && d.delta && d.delta->has_reference() && s.have_origin) {
            const size_t n = d.delta->reference_points();
            const core::delta::Point* ref = d.delta->reference();
            for (size_t i = 0; i < n; ++i) box->pts[i] = ref[i];
            float x = 0.0f, y = 0.0f;
            line_in_frame(s, r, x, y);
            core::track::shift(box->pts, n, -x, -y);
            box->n = static_cast<uint16_t>(n);
            r.ref_ms = s.ref_ms;
        }
        box->rec = r;
        box->op = Op::Save;
        box->slot = static_cast<uint8_t>(s.track);
        s.track_dirty = false;
        box->post();
    }
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


// --- detail log (core/log/detail.hpp) ------------------------------------

// Out of line for logic_tick's frame, which is at the 1 kB limit.
__attribute__((noinline))
void log_command(Deps& d, uint32_t now, const core::state::Command& cmd) {
    if (d.evt_log) d.evt_log->push(core::log::command_event(now, cmd));
}

// This pass's events, and on a Scheduled pass the 50 Hz row of what the
// filters made of the sensors. Its own function, kept out of line, so its
// slot copies are not in logic_tick's frame (CLAUDE.md, 1.28). The scratch
// is LogicState's.
__attribute__((noinline))
void detail_log(Deps& d, LogicState& s, uint32_t now, bool scheduled,
                const core::state::Gnss& g, const core::state::Motion& m,
                const core::state::Engine& e, const core::state::Attitude& att,
                const core::state::Health& health,
                const core::fusion::SpeedInput& fi,
                const core::fusion::SpeedOutput& fo) {
    const bool still = still_for_flash(d);
    const core::state::Lap lap = d.slots->lap.get();

    if (d.evt_log) {
        const core::state::Stunt st = d.slots->stunt.get();
        const core::state::Perf pf = d.slots->perf.get();
        const core::state::Battery bat = d.slots->battery.get();
        core::log::Watched& w = s.watched;
        w.health = health;
        w.fix = g.fix;
        w.sats = g.sats;
        w.hdop = g.hdop;
        w.lap_count = lap.count;
        w.last_lap_ms = lap.last_ms;
        w.last_done_ms = lap.last_done_ms;
        w.last_was_best = lap.last_was_best;
        w.split_at_ms = lap.split_at_ms;
        w.split_sector = lap.split_sector;
        w.split_ms = lap.split_ms;
        w.lap_phase = static_cast<uint8_t>(lap.phase);
        w.airborne = att.airborne;
        w.land_ms = att.land_ms;
        w.airtime_ms = att.airtime_ms;
        w.land_g = att.last_land_g;
        w.sliding = att.sliding;
        w.slide_rate_dps = att.slide_rate_dps;
        w.stunt_seq = st.seq;
        w.stunt_kind = st.kind;
        w.stunt_grade = st.grade;
        w.stunt_ms = st.ms;
        w.stunt_peak_deg = st.peak_deg;
        w.stunt_ft = st.ft;
        w.stunt_points = st.points;
        w.perf_seq = pf.seq;
        w.perf_event = pf.event;
        w.perf_best = pf.event_best;
        if (pf.event < core::state::Perf::kCount) {
            w.perf_ms = pf.last[pf.event].ms;
            w.perf_x = pf.last[pf.event].x;
        }
        w.front_verdict = static_cast<uint8_t>(s.wheels.front());
        w.rear_verdict = static_cast<uint8_t>(s.wheels.rear());
        w.power_fail = d.power_fail.load(std::memory_order_relaxed);
        w.battery_verdict = bat.verdict;
        w.rest_v = bat.rest_v;
        w.crank_v = bat.crank_min_v;
        w.gear = e.gear;
        w.led_faults = e.led_faults;
        w.rpm = e.rpm;
        w.mph = m.best_mph;
        w.moving = !still;
        w.gearcal_phase = static_cast<uint8_t>(s.guide.phase());
        w.fusion_resets = fo.resets;
        w.suspect = fo.suspect;
        {
            const core::state::Heart h = d.slots->heart.get();
            w.hr_link = h.link;
            w.hr_bpm = core::hr::fresh(h, now) ? h.bpm : 0;
            w.hr_no_pulse = core::hr::no_pulse(h, now);
            const core::state::HrRecovery r = d.slots->hr_recovery.get();
            w.hrr_seq = r.seq;
            w.hrr_from_bpm = r.from_bpm;
            w.hrr_drop1 = r.drop1;
            w.hrr_drop2 = r.has_drop2 ? r.drop2 : 0;
            w.hrr_has2 = r.has_drop2;
        }
        w.dropped[core::log::kMain] = d.blackbox ? d.blackbox->dropped() : 0;
        w.dropped[core::log::kImu] = d.imu_log ? d.imu_log->dropped() : 0;
        w.dropped[core::log::kTach] = d.tach_log ? d.tach_log->dropped() : 0;
        w.dropped[core::log::kDet] = d.det_log ? d.det_log->dropped() : 0;
        w.dropped[core::log::kEvt] = d.evt_log->dropped();
        const size_t n = s.events.watch(w, now, s.event_out,
                                        core::log::EventWatcher::kMaxPerPass);
        for (size_t i = 0; i < n; ++i) d.evt_log->push(s.event_out[i]);
    }

    s.det_gps_new |= fi.gps_new;
    s.det_used |= fo.used;
    s.det_rejected |= fo.rejected;
    if (d.det_log && scheduled) {
        core::log::DetRow& r = s.det;
        r = core::log::DetRow{};
        r.t_ms = now;
        r.shown_mph = m.best_mph;
        r.front_mph = m.front_mph;
        r.rear_mph = m.rear_mph;
        r.gps_mph = g.speed_mph;
        r.hdop = g.hdop;
        r.gps_new = s.det_gps_new;
        r.fused_mph = fo.mph;
        r.fused_sigma = fo.sigma_mph;
        r.fused_valid = fo.valid;
        r.used = s.det_used;
        r.rejected = s.det_rejected;
        s.det_gps_new = false;
        s.det_used = s.det_rejected = 0;
        r.suspect = fo.suspect;
        r.resets = fo.resets;
        r.accel_g = fi.accel_g;
        r.accel_bias_g = fo.bias_g;
        r.front_verdict = static_cast<uint8_t>(s.wheels.front());
        r.rear_verdict = static_cast<uint8_t>(s.wheels.rear());
        r.lean_deg = att.lean_deg;
        r.accel_lean_deg = att.accel_lean_deg;
        r.pitch_deg = att.pitch_deg;
        r.steer_deg = att.steer_deg;
        r.yaw_rate_dps = att.yaw_rate_dps;
        r.slide_rate_dps = att.slide_rate_dps;
        r.heading_deg = att.heading_deg;
        r.sliding = att.sliding;
        r.airborne = att.airborne;
        r.g_lat = att.g_lat;
        r.g_lon = att.g_lon;
        r.g_vert = att.g_vert;
        r.bias_gx = att.bias_gx_dps;
        r.bias_gy = att.bias_gy_dps;
        r.bias_gz = att.bias_gz_dps;
        r.ground_deg = s.stunt.ground_deg();
        r.lift_deg = s.stunt.lift_deg();
        r.stunt_kind = d.slots->stunt.get().live_kind;
        r.rpm = e.rpm;
        r.ratio = m.best_mph > 3.0f ? e.rpm / m.best_mph : 0.0f;
        r.gear = e.gear;
        r.neutral = e.neutral;
        r.shift_mask = e.shift_mask;
        r.lap_phase = static_cast<uint8_t>(lap.phase);
        r.dist_to_line_ft = lap.dist_to_line_ft;
        r.perf_phase = d.slots->perf.get().phase;
        r.gearcal_phase = static_cast<uint8_t>(s.guide.phase());
        r.vbat = d.slots->elec.get().vbat;
        r.still = still;
        d.det_log->push(r);
    }
}

// One blackbox row. Its own function, kept out of line like detail_log,
// so the 100-byte Record is not in logic_tick's frame, which at -O2 sits
// close to the 1 kB limit (CLAUDE.md, 1.28).
__attribute__((noinline))
void blackbox_row(Deps& d, LogicState& s, uint32_t now,
                  const core::state::Gnss& g, const core::state::Motion& m,
                  const core::state::Engine& e, const core::state::Attitude& a,
                  const core::fusion::SpeedOutput& fo,
                  uint32_t lap_done_ms, uint16_t lap_done_index) {
    core::log::Record rec;
    rec.front_mph = m.front_mph;
    rec.rear_mph = m.rear_mph;
    rec.gps_mph = g.speed_mph;
    rec.hdop = g.hdop;
    rec.gps_utc_ms = g.utc_ms;
    rec.fused_mph = fo.mph;
    rec.fused_sigma = fo.sigma_mph;
    rec.alt_ft = g.alt_m * 3.28084f;
    {
        const core::state::Heart h = d.slots->heart.get();
        rec.hr_bpm = core::hr::fresh(h, now) ? h.bpm : 0;
    }
    {
        const core::state::Tpms t = d.slots->tpms.get();
        rec.front_psi = t.front.valid ? t.front.psi : 0.0f;
        rec.rear_psi = t.rear.valid ? t.rear.psi : 0.0f;
    }
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
    if (s.nav.following() && s.nav_progress.on) {
        rec.nav_turn_ft = s.nav_progress.to_cue_dm * 0.328084f;
        rec.nav_dest_mi = s.nav_progress.remain_dm / 16093.44f;
        rec.limit_mph = s.nav_progress.mph;
    }
    rec.vbat = d.slots->elec.get().vbat;
    rec.shift_lamps = static_cast<uint8_t>(
        __builtin_popcount(static_cast<unsigned>(e.shift_mask | e.shift_hold)));
    {
        // As ui::App::update_slip, without the hold: a log row is the moment.
        const core::state::Health h = d.slots->health.get();
        core::slip::Config sc;
        sc.hold_ms = 0;
        const core::slip::Reading r = s.log_slip.update(
            m.front_mph, h[core::state::Dev::FrontWheel].state != core::state::DevState::Fault,
            m.rear_mph, h[core::state::Dev::RearWheel].state != core::state::DevState::Fault,
            fo.valid ? fo.mph : g.speed_mph, fo.valid || g.fix, now, sc);
        rec.slip_pct = static_cast<int8_t>(r.kind == core::slip::Kind::Spin ? r.pct : -r.pct);
    }
    if (e.ecu_online) {
        rec.tps_pct = static_cast<uint8_t>(std::lround(e.tps_pct));
        rec.map_kpa = static_cast<uint8_t>(std::lround(e.map_kpa));
        rec.engine_f = static_cast<int16_t>(std::lround(core::units::f_from_c(e.engine_c)));
        rec.intake_f = static_cast<int16_t>(std::lround(core::units::f_from_c(e.intake_c)));
        rec.inj_raw = e.inj_raw;
    }
    d.blackbox->push(rec);
}

// --- routing ---------------------------------------------------------------

core::route::Point to_point(double lat, double lon) {
    return {static_cast<int32_t>(std::lround(lat * 1e7)), static_cast<int32_t>(std::lround(lon * 1e7))};
}

// Where the bike is now: the last fix carried forward along its course at
// the fused speed. The GPS is at 1 Hz (bringup), and a cue a second late
// at 50 mph is 70 ft past the turn. Not past 2 s: a stale fix is not news.
core::route::Point dead_reckon(const core::state::Gnss& g, const core::state::Fused& f, uint32_t now) {
    double lat = g.lat, lon = g.lon;
    const uint32_t age = now - g.rx_ms;
    const float mph = f.speed_valid ? f.speed_mph : g.speed_mph;
    constexpr float kMinCourseMph = 3.0f;    // below this the GPS course is noise
    if (g.fix && mph > kMinCourseMph && age < 2000) {
        constexpr double kPi = 3.14159265358979323846;
        const double m = mph * 0.44704 * age / 1000.0;
        const double c = g.course_deg * kPi / 180.0;
        lat += m * std::cos(c) / 111195.0;
        lon += m * std::sin(c) / (111195.0 * std::cos(g.lat * kPi / 180.0));
    }
    return to_point(lat, lon);
}

// The picker's list: the waypoints that are set, then what the card gave
// (favourites, then the nearest fuel and food).
// Out of line: its locals (a whole Nav) must not land in logic_tick's frame.
__attribute__((noinline)) void build_nav_list(LogicState& s) {
    using core::state::NavDest;
    uint8_t n = 0;
    // What the phone sent leads: it is the newest thing the rider asked for.
    if (s.have_phone_dest) s.nav_list[n++] = s.phone_dest;
    for (size_t i = 0; i < core::waypoint::kCount && n < core::state::Nav::kMaxDest; ++i) {
        const core::state::Waypoint& w = s.waypoints.pts[i];
        if (!w.set) continue;
        NavDest& o = s.nav_list[n++];
        o = NavDest{};
        std::snprintf(o.name, sizeof o.name, "%s", core::waypoint::name(i));
        const core::route::Point p = to_point(w.lat, w.lon);
        o.lat_e7 = p.lat_e7;
        o.lon_e7 = p.lon_e7;
        o.kind = NavDest::Waypoint;
        o.waypoint = static_cast<uint8_t>(i);
    }
    for (uint8_t i = 0; i < s.n_dests && n < core::state::Nav::kMaxDest; ++i) {
        const core::route::Dest& d = s.dests[i];
        NavDest& o = s.nav_list[n++];
        o = NavDest{};
        std::memcpy(o.name, d.name, sizeof o.name);
        o.name[sizeof o.name - 1] = '\0';
        o.lat_e7 = d.p.lat_e7;
        o.lon_e7 = d.p.lon_e7;
        o.kind = d.kind == core::route::PoiFuel ? NavDest::Fuel
               : d.kind == core::route::PoiFood ? NavDest::Food
               : d.kind == core::route::kDestRecent ? NavDest::Recent : NavDest::Favourite;
    }
    s.nav_list_n = n;
}

// The route into the MAP screen's buffer, in the trip trace's frame, every
// nth point so it fits (the last one always); or empty, for no route. A
// point's speed_mph is 65 when any of the route since the point before is
// on a road over the speed limit (a detour), so the map draws that stretch
// amber however thin.
__attribute__((noinline)) void trace_route(Deps& d, LogicState& s, const core::route::Route* r) {
    s.route_traced = r != nullptr;
    if (!d.route_trace) return;
    RouteTrace& t = *d.route_trace;
    t.pts.clear();
    if (r && r->n_pts >= 2) {
        const auto ll = [&](uint32_t i) {
            return core::geo::LatLon{r->pts[i].p.lat_e7 * 1e-7, r->pts[i].p.lon_e7 * 1e-7};
        };
        ensure_origin(s, ll(0));
        const uint32_t step = (r->n_pts + RouteTrace::kPoints - 2) / (RouteTrace::kPoints - 1);
        uint32_t prev = 0;
        for (uint32_t i = 0;; i += step) {
            if (i >= r->n_pts) i = r->n_pts - 1;
            core::state::TrackPoint p;
            to_local_ft(s.origin, ll(i), p.x_ft, p.y_ft);
            for (uint32_t k = prev; k < i; ++k)   // segment k runs from point k to k+1
                if (!core::route::allowed(r->pts[k].flags, r->pts[k].cap_mph, s.nav.limit())) p.speed_mph = 65.0f;
            prev = i;
            t.pts.push(p);
            if (i == r->n_pts - 1) break;
        }
    }
    t.gen.fetch_add(1, std::memory_order_release);
}

// The route's road names (RouteData::labels) into the frame the map draws
// in, once per route; publish_nav copies them every frame.
void label_route(LogicState& s, const RouteData* rd) {
    s.n_route_labels = 0;
    if (!rd) return;
    for (uint8_t i = 0; i < rd->n_labels && i < core::state::Nav::kMaxLabels; ++i) {
        if (!rd->label_names[i][0] || rd->labels[i].point >= rd->route.n_pts) continue;
        const core::route::Point& at = rd->route.pts[rd->labels[i].point].p;
        core::state::Nav::Label& l = s.route_labels[s.n_route_labels++];
        to_local_ft(s.origin, core::geo::LatLon{at.lat_e7 * 1e-7, at.lon_e7 * 1e-7}, l.x_ft, l.y_ft);
        std::snprintf(l.name, sizeof l.name, "%s", rd->label_names[i]);
    }
}

void search_go(LogicState& s, core::route::Point to, const char* name, const core::state::Gnss& g,
               core::route::Point here);

// Into the recent list on the card, on the next free turn of the mailbox.
void remember(LogicState& s, core::route::Point to, const char* name) {
    s.recent = core::route::Dest{};
    std::snprintf(s.recent.name, sizeof s.recent.name, "%.15s", name);
    s.recent.p = to;
    s.recent_want = true;
}

// The mailbox: take an answer, then post the next job, one at a time.
// Order: the map first, then a route the navigator wants, then a fresh
// destination list.
// Out of line: its locals (a whole Nav) must not land in logic_tick's frame.
__attribute__((noinline)) void pump_route(Deps& d, LogicState& s, const core::state::Gnss& g,
                core::route::Point here, uint32_t now) {
    RouteMailbox* box = d.route;
    if (!box) return;
    using Op = RouteMailbox::Op;
    if (box->answered()) {
        s.nav.set_map(box->map_ok);
        s.map_date = box->map_date;
        if (box->op == Op::Destinations) {
            s.n_dests = box->n_dests;
            for (uint8_t i = 0; i < box->n_dests; ++i) s.dests[i] = box->dests[i];
        } else if (box->op == Op::Search) {
            s.searching = false;
            s.search_ok = box->search_ok;
            // An answer to what is still typed; otherwise ask again.
            if (std::strcmp(box->query, s.query) == 0) {
                s.search_next = box->next;
                s.search_matches = box->matches;
                s.n_hits = box->n_hits;
                for (uint8_t i = 0; i < box->n_hits; ++i) s.hits[i] = box->hits[i];
            } else {
                s.search_want = true;
            }
        } else if (box->op == Op::SaveRecent) {
            s.dests_asked = false;          // the picker shows it at once
        } else if (box->op == Op::House) {
            s.searching = false;
            if (s.search == core::state::Nav::SearchNumber && box->house_at.lat_e7 != 0) {
                char label[24];
                std::snprintf(label, sizeof label, "%.7s %.15s", s.digits, s.picked.name);
                search_go(s, box->house_at, label, g, here);
            }
        } else if (box->op == Op::Query) {
            const uint8_t into = box->into & 1;
            const core::route::Route& r = box->data[into].route;
            if (s.nav.answered(box->status, r, now)) {
                box->live = into;
                trace_route(d, s, &r);
                label_route(s, &box->data[into]);
            }
        }
        box->release();
    }
    if (!s.nav.following()) {
        box->live = 0xFF;
        if (s.route_traced) trace_route(d, s, nullptr);
        label_route(s, nullptr);
    }
    if (!box->free()) return;

    // A card without a map is looked at again every 30 s: one put in, or a
    // map copied on, is found without a restart.
    if (!s.map_asked || (!s.nav.has_map() && now - s.map_asked_ms >= 30000)) {
        s.map_asked = true;
        s.map_asked_ms = now;
        box->op = Op::Open;
        box->post();
        return;
    }
    core::route::Request q;
    if (s.nav.take_request(q)) {
        box->req = q;
        box->into = box->live == 0 ? 1 : 0;
        box->op = Op::Query;
        box->post();
        return;
    }
    if (s.recent_want) {
        s.recent_want = false;
        box->recent = s.recent;
        box->op = Op::SaveRecent;
        box->post();
        return;
    }
    if (s.house_want) {
        s.house_want = false;
        s.searching = true;
        box->house_entry = s.picked.entry;
        box->house_number = static_cast<uint32_t>(std::atoi(s.digits));
        box->op = Op::House;
        box->post();
        return;
    }
    if (s.search_want && s.search == core::state::Nav::SearchTyping) {
        s.search_want = false;
        s.searching = true;
        std::memcpy(box->query, s.query, sizeof box->query);
        box->near = here;
        box->op = Op::Search;
        box->post();
        return;
    }
    // "Nearest" drifts as the bike moves: once a minute is plenty.
    if (g.fix && s.nav.has_map() && (!s.dests_asked || now - s.dests_asked_ms >= 60000)) {
        s.dests_asked = true;
        s.dests_asked_ms = now;
        box->near = here;
        box->op = Op::Destinations;
        box->post();
    }
}

// "Where to?" ----------------------------------------------------------------

// Route to a search result; the search closes once routing has begun.
// Without a fix it stays open: the header says why.
void search_go(LogicState& s, core::route::Point to, const char* name, const core::state::Gnss& g,
               core::route::Point here) {
    if (!s.nav.start(to, here, g.fix)) return;
    std::snprintf(s.nav_target, sizeof s.nav_target, "%.15s", name);   // the target line is narrow
    s.search = core::state::Nav::SearchOff;
    remember(s, to, name);
}

__attribute__((noinline)) void search_command(LogicState& s, const core::state::Command& cmd,
                                              const core::state::Gnss& g, core::route::Point here) {
    using core::state::CommandId;
    using core::state::Nav;
    switch (cmd.id) {
        case CommandId::NavSearch:
            s.search = cmd.i ? Nav::SearchTyping : Nav::SearchOff;
            s.query[0] = '\0';
            s.n_hits = 0;
            s.search_want = cmd.i != 0;
            break;
        case CommandId::NavSearchKey: {
            const char c = static_cast<char>(cmd.i);
            if (s.search == Nav::SearchTyping) {
                size_t n = std::strlen(s.query);
                if (c == 8) {
                    if (n) s.query[n - 1] = '\0';
                } else if (n < core::route::kKeyLen && c && std::strchr(core::route::kAlphabet, c) &&
                           !(c == ' ' && (n == 0 || s.query[n - 1] == ' '))) {
                    // A letter after a house number (3+ digits): the space
                    // goes in by itself, "13511R" is "13511 R", as a
                    // Garmin's address entry takes the number then the street.
                    const bool letter = c >= 'A' && c <= 'Z';
                    if (letter && n >= 3 && n + 1 < core::route::kKeyLen &&
                        std::strspn(s.query, "0123456789") == n)
                        s.query[n++] = ' ';
                    s.query[n] = c;
                    s.query[n + 1] = '\0';
                }
                s.search_want = true;
            } else if (s.search == Nav::SearchNumber) {
                size_t n = std::strlen(s.digits);
                if (c == 8) {
                    if (n) s.digits[n - 1] = '\0';
                    else s.search = Nav::SearchTyping;     // delete past the number: back to the street
                } else if (c >= '0' && c <= '9' && n + 1 < sizeof s.digits && !(n == 0 && c == '0')) {
                    s.digits[n] = c;
                    s.digits[n + 1] = '\0';
                }
            }
            break;
        }
        case CommandId::NavSearchPick:
            if (s.search != Nav::SearchTyping || cmd.i < 0 || cmd.i >= s.n_hits) break;
            if (s.hits[cmd.i].kind == core::route::Place::Street && s.hits[cmd.i].nums_n) {
                s.picked = s.hits[cmd.i];
                s.digits[0] = '\0';
                s.search = Nav::SearchNumber;
            } else {
                search_go(s, s.hits[cmd.i].p, s.hits[cmd.i].name, g, here);
            }
            break;
        case CommandId::NavSearchGo:
            if (s.search != Nav::SearchNumber) break;
            if (s.digits[0]) s.house_want = true;               // the card places the number
            else search_go(s, s.picked.p, s.picked.name, g, here);
            break;
        default:
            break;
    }
}

// A destination from the phone: into the picker, and routed to straight
// away, since typing it was the rider asking to go there. Without a fix or
// a map it waits in the list.
__attribute__((noinline)) void phone_destination(Deps& d, LogicState& s, const core::state::Gnss& g,
                                                 core::route::Point here) {
    const core::state::PhoneDest pd = d.slots->phone_dest.get();
    if (pd.seq == s.phone_seq) return;
    s.phone_seq = pd.seq;
    core::state::NavDest& o = s.phone_dest;
    o = core::state::NavDest{};
    std::memcpy(o.name, pd.name, sizeof o.name);
    o.name[sizeof o.name - 1] = '\0';
    o.lat_e7 = pd.lat_e7;
    o.lon_e7 = pd.lon_e7;
    o.kind = core::state::NavDest::Phone;
    s.have_phone_dest = true;
    if (s.nav.start(core::route::Point{o.lat_e7, o.lon_e7}, here, g.fix)) {
        std::memcpy(s.nav_target, o.name, sizeof s.nav_target);
        remember(s, core::route::Point{o.lat_e7, o.lon_e7}, o.name);
    }
}

__attribute__((noinline)) void nav_go(Deps& d, LogicState& s, const core::state::Gnss& g,
                                      int32_t i, uint32_t now) {
    if (i < 0 || i >= s.nav_list_n) return;
    const core::state::NavDest& to = s.nav_list[i];
    const core::state::Fused fu = d.slots->fused.get();
    if (s.nav.start(core::route::Point{to.lat_e7, to.lon_e7}, dead_reckon(g, fu, now), g.fix))
        std::memcpy(s.nav_target, to.name, sizeof s.nav_target);
}

// Out of line: its locals (a whole Nav) must not land in logic_tick's frame.
__attribute__((noinline)) void publish_nav(Deps& d, LogicState& s) {
    build_nav_list(s);
    core::state::Nav& n = s.nav_out;
    n = core::state::Nav{};
    n.phase = static_cast<uint8_t>(s.nav.phase());
    n.status = static_cast<uint8_t>(s.nav.status());
    n.map_date = s.nav.has_map() ? s.map_date : 0;
    n.avoid_dirt = s.nav.avoid_dirt();
    n.max_mph = s.nav.max_mph();
    for (uint8_t i = 0; i < s.nav_list_n; ++i) n.dest[i] = s.nav_list[i];
    n.n_dest = s.nav_list_n;
    std::memcpy(n.target, s.nav_target, sizeof n.target);
    RouteMailbox* box = d.route;
    const core::route::Progress& p = s.nav_progress;
    if (box && box->live < 2 && s.nav.following()) {
        const RouteData& rd = box->data[box->live];
        n.following = true;
        for (uint8_t i = 0; i < s.n_route_labels; ++i) n.labels[i] = s.route_labels[i];
        n.n_labels = s.n_route_labels;
        if (p.on || p.cue != core::route::kNone) {
            if (p.cue < rd.route.n_cues) {
                const core::route::Cue& c = rd.route.cues[p.cue];
                n.cue_kind = static_cast<uint8_t>(c.kind);
                n.cue_turn_deg = c.turn_deg;
                std::memcpy(n.cue_road, rd.cue_names[p.cue], sizeof n.cue_road);
                n.cue_road[sizeof n.cue_road - 1] = '\0';
            }
            n.to_cue_ft = p.to_cue_dm * 0.328084f;
            n.remain_mi = p.remain_dm / 16093.44f;
            n.remain_s = p.remain_ds / 10;
            n.limit_mph = p.mph;
            n.over_limit = !core::route::allowed(p.flags, p.cap_mph, s.nav.limit());
            n.off_route = p.off_route;
            if (p.on && !p.off_route) {
                // The bends in the next 800 m (core/route/pace.hpp).
                const size_t np = core::route::pace_notes(rd.route, p.along_dm, p.along_dm + 8000,
                                                          s.pace, core::state::Nav::kMaxPace);
                uint32_t prev_end = p.along_dm;
                for (size_t i = 0; i < np; ++i) {
                    const core::route::PaceNote& pn = s.pace[i];
                    core::route::format_note(pn, n.pace[i], sizeof n.pace[i]);
                    n.pace_gap_ft[i] = pn.at_dm > prev_end ? (pn.at_dm - prev_end) * 0.328084f : 0.0f;
                    n.pace_into[i] = pn.flags & core::route::kPaceInto;
                    prev_end = pn.at_dm + pn.len_dm;
                }
                n.n_pace = static_cast<uint8_t>(np);
            }
        } else {
            // No fix yet on this route: the whole of it is ahead.
            n.remain_mi = rd.route.total_dm / 16093.44f;
            n.remain_s = rd.route.total_ds / 10;
        }
    }
    n.usb_refused_seq = s.usb_refused_seq;
    n.search = s.search;
    if (s.search != core::state::Nav::SearchOff) {
        n.search_ok = s.search_ok;
        n.searching = s.searching || s.search_want;
        std::memcpy(n.query, s.query, sizeof n.query);
        n.next = s.search_next;
        n.matches = s.search_matches;
        n.n_hits = s.n_hits < core::state::Nav::kMaxHits ? s.n_hits : core::state::Nav::kMaxHits;
        for (uint8_t i = 0; i < n.n_hits; ++i) {
            core::state::Nav::Hit& h = n.hits[i];
            std::memcpy(h.name, s.hits[i].name, sizeof h.name);
            std::memcpy(h.city, s.hits[i].city, sizeof h.city);
            h.kind = static_cast<uint8_t>(s.hits[i].kind);
            h.numbers = s.hits[i].nums_n != 0;
            h.dist_mi = s.hits[i].dist_m / 1609.344f;
        }
        std::memcpy(n.street, s.picked.name, sizeof n.street);
        std::memcpy(n.digits, s.digits, sizeof n.digits);
    }
    d.slots->nav.publish(n);
}

}  // namespace

void logic_restore(Deps& d, LogicState& s) {
    // The odometer and engine hours come back from NVS. Everything else
    // starts at zero, because everything else is a trip, not a life.
    if (!d.storage) return;
    double odo = 0.0, hours = 0.0;
    const hal::Status so = d.storage->read_f64(hal::kNsLife, hal::kKeyOdometerMi, odo);
    const hal::Status sh = d.storage->read_f64(hal::kNsLife, hal::kKeyEngineHours, hours);
    // NotPresent is a new board. Anything else is a total that is still in
    // flash and was not read: never save this session's zero over it.
    const auto unread = [](hal::Status s) { return !hal::ok(s) && s != hal::Status::NotPresent; };
    if (unread(so) || unread(sh)) d.totals_unread.store(true, std::memory_order_relaxed);
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
    s.gnss_now = d.slots->gnss.get();
    const core::state::Gnss&  g = s.gnss_now;
    const core::state::Motion m = d.slots->motion.get();
    const core::state::Engine e = d.slots->engine.get();

    // The lap timer's thresholds: the crossing gates, and the speed a Set
    // line needs.
    core::lap::Config lc;
    if (d.settings) {
        const core::config::LapView lv = core::config::lap_view(*d.settings);
        lc.half_width_ft = lv.half_width_ft;
        lc.min_speed_mph = lv.min_speed_mph;
        lc.min_lap_ms = lv.min_lap_ms;
    }
    if (d.slots->health.get()[core::state::Dev::Gnss].limited) {
        lc.max_fix_gap_ms = core::lap::kMaxFixGap1HzMs;
    }

    // --- commands -------------------------------------------------------
    core::state::Command cmd;
    const bool detail = d.settings && d.settings->detail_log;
    while (commands.take(cmd)) {
        // Every command, as it is acted on: what the rider pressed and every
        // edit, with what it was set to.
        if (detail) log_command(d, now, cmd);
        switch (cmd.id) {
            case core::state::CommandId::LapSetLine:
                // The screen asked; this task decides whether the fix is
                // good enough, because the screen only has a Frame. Moving,
                // too: the line faces the way the GPS course points.
                if (!core::lap::may_set_line(g.fix, g.speed_mph, lc)) {
                    ++s.line_refused_seq;
                } else {
                    s.lap.set_line(core::geo::LatLon{g.lat, g.lon}, g.course_deg);
                    // The splits were placed round the old line, and the
                    // reference lap was ridden over it.
                    forget_line_state(d, s);
                    ensure_origin(s, core::geo::LatLon{g.lat, g.lon});
                    adopt_new_line(d, s,
                                   core::track::Line{core::geo::LatLon{g.lat, g.lon},
                                                     g.course_deg},
                                   gps_today(g));
                }
                break;
            case core::state::CommandId::LapArm:
                s.lap.arm();
                s.sectors.abort_lap();
                s.lap_peaks.reset();
                break;
            case core::state::CommandId::PerfReset:
                s.perf.reset_bests();
                break;
            case core::state::CommandId::StuntReset:
                s.stunt.reset_session();
                break;
            case core::state::CommandId::ShiftLampTest:
                d.lamp_test_req.fetch_add(1, std::memory_order_relaxed);
                break;
            case core::state::CommandId::LayoutStock:
                // `storage` clears ui/active; render then shows stock.
                if (d.layout) d.layout->stock_req.store(true, std::memory_order_relaxed);
                break;
            case core::state::CommandId::CrashTest:
                s.crash.test(now);
                break;
            case core::state::CommandId::CrashCancel:
                s.crash.cancel();
                d.slots->crash.publish(s.crash.state());
                break;
            case core::state::CommandId::LeanPeaksReset:
                s.session_peaks.lean_left_deg = 0.0f;
                s.session_peaks.lean_right_deg = 0.0f;
                break;
            case core::state::CommandId::TrackForget:
                if (s.track >= 0 && d.tracks && s.tracks_known) {
                    d.tracks->lib.rec[s.track] = core::track::Record{};
                    s.forget_slot = s.track;
                    s.track_dirty = false;
                }
                [[fallthrough]];
            case core::state::CommandId::LapClear:
                // The saved track stays on the card (unless forgotten just
                // above); it just does not come straight back.
                s.track = -1;
                s.track_recalled = false;
                s.track_unsaved = false;
                s.recall_off = true;
                s.ref_ms = 0;
                s.want_ref = false;
                s.lap.clear();
                s.sectors.reset();
                s.last_split = core::lap::Split{};
                s.last_done_ms = 0;
                s.last_was_best = false;
                s.lap_count = 0;
                if (d.lap_history) *d.lap_history = LapHistory{};
                if (d.lap_trace) d.lap_trace->clear();
                if (d.delta) d.delta->clear();
                break;
            case core::state::CommandId::TripReset:
                if (cmd.i == 1) s.trip.reset_trip_b();
                else            s.trip.reset_trip_a();
                break;
            case core::state::CommandId::ServiceDone:
                if (d.settings) {
                    d.settings->service_at_h = static_cast<float>(s.trip.engine_hours());
                }
                break;
            case core::state::CommandId::OdoSet:
            case core::state::CommandId::EngHoursSet:
                // The laptop's: after an NVS erase, a board swap, or totals
                // that would not read back. Negative or NaN is refused. Once
                // both are given they are the truth, and may be saved again.
                if (cmd.f >= 0.0f) {
                    if (cmd.id == core::state::CommandId::OdoSet) {
                        s.trip.restore(cmd.f, s.trip.engine_hours());
                        s.odo_set = true;
                    } else {
                        s.trip.restore(s.trip.odo_mi(), cmd.f);
                        s.hours_set = true;
                    }
                    if (s.odo_set && s.hours_set) {
                        d.totals_unread.store(false, std::memory_order_relaxed);
                    }
                }
                break;
            case core::state::CommandId::ConfigSet:
                // Every edit lands here: the CONFIG screen, the Wi-Fi page
                // and imu's Calibrate level all post ConfigSet, and this is
                // Settings' one writer. Then every module holding a copy
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
            case core::state::CommandId::TpmsLearnFront:
            case core::state::CommandId::TpmsLearnRear:
                d.slots->ble_request.publish(core::state::BleRequest{
                    cmd.id == core::state::CommandId::TpmsLearnFront
                        ? core::state::BleRequest::TpmsLearnFront
                        : core::state::BleRequest::TpmsLearnRear});
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
            case core::state::CommandId::NavGo:
                nav_go(d, s, g, cmd.i, now);
                break;
            case core::state::CommandId::NavCancel:
                s.nav.cancel();
                break;
            case core::state::CommandId::NavAnySpeed:
                s.nav.any_speed();
                break;
            case core::state::CommandId::NavSearch:
            case core::state::CommandId::NavSearchKey:
            case core::state::CommandId::NavSearchPick:
            case core::state::CommandId::NavSearchGo:
                search_command(s, cmd, g, dead_reckon(g, d.slots->fused.get(), now));
                break;
            case core::state::CommandId::ImuCalibrate:
                // `imu` has the samples; it answers with ConfigSets for the
                // mount offset, which land back here.
                d.slots->imu_request.publish(core::state::ImuRequest{
                    core::state::ImuRequest::CalibrateLevel});
                break;
            case core::state::CommandId::StorageEject:
                d.slots->storage_request.publish(core::state::StorageRequest{
                    core::state::StorageRequest::Eject});
                break;
            case core::state::CommandId::StorageToHost: {
                // Parked only: the card goes to the laptop and the logger
                // stops while it is there. The fused speed when it is good;
                // else the GPS's, and only from a good fix, since a parked
                // receiver's drift reads a few mph.
                const core::state::Fused fu = d.slots->fused.get();
                const bool moving = fu.speed_valid ? fu.speed_mph >= 3.0f
                                  : (g.fix && g.hdop <= 2.5f && g.speed_mph >= 3.0f);
                if (!moving) {
                    d.slots->storage_request.publish(core::state::StorageRequest{
                        core::state::StorageRequest::UsbDrive});
                } else {
                    ++s.usb_refused_seq;
                }
                break;
            }
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
    fused.rejected = fo.rejected;
    fused.suspect = fo.suspect;
    fused.resets = fo.resets;
    fused.accel_bias_g = fo.bias_g;
    d.slots->fused.publish(fused);

    // --- routing ----------------------------------------------------------
    {
        const core::route::Point here = dead_reckon(g, fused, now);
        RouteMailbox* box = d.route;
        const core::route::Route* live =
            box && box->live < 2 ? &box->data[box->live].route : nullptr;
        // The setting, kept in NVS by keeper like any other (CONFIG -> Nav).
        if (d.settings) {
            s.nav.set_avoid_dirt(d.settings->nav_avoid_dirt);
            s.nav.set_max_mph(core::config::nav_limit_mph(d.settings->nav_speed_limit));
        }
        phone_destination(d, s, g, here);
        s.nav_progress = s.nav.tick(live, here, g.fix, now);
        pump_route(d, s, g, here, now);
        if (scheduled) publish_nav(d, s);
    }

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

    // --- saved tracks ---------------------------------------------------
    pump_tracks(d, s, now);
    if (d.tracks && s.tracks_known) {
        // A line set before the library came off the card is placed now.
        if (s.track_unsaved && s.lap.has_line()) {
            adopt_new_line(d, s, s.pending_line, s.pending_day);
        }
        if (!s.recall_off && s.track < 0 && !s.lap.has_line() &&
            g.rx_ms != s.last_lap_rx_ms && core::waypoint::good_fix(g)) {
            const int slot = core::track::nearest(d.tracks->lib, core::geo::LatLon{g.lat, g.lon});
            if (slot >= 0) recall_track(d, s, slot, g);
        }
    }

    // --- trip -----------------------------------------------------------
    core::trip::Input ti;
    ti.speed_mph = m.best_mph;
    ti.engine_on = e.rpm > 0.0f;
    ti.tps_pct = e.tps_pct;
    ti.t_ms = now;
    s.trip.update(ti, s.fuel);
    core::state::Trip trip = s.trip.publish();
    trip.service_due = d.settings &&
        core::trip::service_due(s.trip.engine_hours(), d.settings->service_at_h,
                                d.settings->service_every_h);
    d.slots->trip.publish(trip);

    // --- session --------------------------------------------------------
    {
        core::session::Input si;
        si.t_ms = now;
        si.speed_mph = m.best_mph;
        si.engine_on = e.rpm > 0.0f;
        si.imu_ok = health.ok(core::state::Dev::Imu);
        si.land_ms = att.land_ms;
        si.airtime_ms = att.airtime_ms;
        si.land_g = att.last_land_g;
        s.session.update(si);
        d.slots->session.publish(s.session.publish());
    }

    // --- performance timers ---------------------------------------------
    // Every pass: a threshold is interpolated between passes, so more of
    // them is finer timing, and the fused speed is fresh on each one.
    // Demo mode: not run, so the slot keeps its last result.
    const bool demo = d.settings && d.settings->demo_mode;
    if (!demo) {
        const bool metric = d.settings && d.settings->unit_system ==
                                              static_cast<uint8_t>(core::units::System::Metric);
        core::perf::Input pin;
        pin.t_ms = now;
        pin.mph = fo.mph;
        pin.valid = fo.valid;
        s.perf.update(pin, core::perf::for_units(metric));
        d.slots->perf.publish(s.perf.publish(metric));
    }

    // --- heart-rate recovery ---------------------------------------------
    // Every pass, off the same fused speed as the timers: parked after a
    // ride, the pulse at the stop against a minute later.
    {
        const core::state::Heart h = d.slots->heart.get();
        const core::state::Engine eng = d.slots->engine.get();
        core::hr::RecoveryInput ri;
        ri.t_ms = now;
        ri.mph = fo.mph;
        ri.speed_ok = fo.valid;
        ri.neutral = eng.neutral;
        ri.engine_running = eng.rpm > 0.0f;
        ri.bpm = core::hr::fresh(h, now) ? h.bpm : 0;
        s.hr_recovery.update(ri);
        d.slots->hr_recovery.publish(s.hr_recovery.publish());
    }

    // --- wheelies and stoppies -------------------------------------------
    // Scheduled passes only: points are an integral over time, and an event
    // pass would sample the same IMU reading again.
    if (scheduled && !demo) {
        core::stunt::Input sti;
        sti.t_ms = now;
        sti.pitch_deg = att.pitch_deg;
        sti.lean_deg = att.lean_deg;
        sti.g_lon = att.g_lon;
        sti.g_lat = att.g_lat;
        sti.g_vert = att.g_vert;
        sti.airborne = att.airborne;
        sti.imu_ok = health.ok(core::state::Dev::Imu);
        sti.mph = fo.mph;
        sti.speed_valid = fo.valid;
        sti.gps_fix = g.fix;
        sti.gps_alt_m = g.alt_m;
        s.stunt.update(sti, core::stunt::Config{});
        d.slots->stunt.publish(s.stunt.publish());
    }

    // --- crash and SOS --------------------------------------------------
    // Scheduled passes: its timers count ticks of real time, and `ble`
    // sends what this publishes.
    if (scheduled) {
        core::crash::Input ci;
        ci.t_ms = now;
        ci.enabled = d.settings && d.settings->crash_sos;
        ci.imu_ok = health.ok(core::state::Dev::Imu);
        ci.mph = m.best_mph;
        ci.hit_g = att.hit_g;
        ci.hit_ms = att.hit_ms;
        ci.g_lat = att.g_lat;
        ci.g_lon = att.g_lon;
        ci.g_vert = att.g_vert;
        d.slots->crash.publish(s.crash.update(ci));
    }

    // --- lap ------------------------------------------------------------

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
    // silence longer than the timer's own gap gate is told to it as no fix,
    // so a lap across the dropout is marked rather than scored as if nothing
    // happened. Not sooner: at 1 Hz a fix a few ms late was a broken segment.
    const bool new_fix = g.rx_ms != s.last_lap_rx_ms;
    const bool silent = static_cast<int32_t>(now - g.rx_ms) >
                        static_cast<int32_t>(lc.max_fix_gap_ms);
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
        const core::lap::Split sp = s.sectors.update(lf, g.course_deg, lu, lc);
        if (sp.crossed) s.last_split = sp;
        if (s.track >= 0 && d.tracks) {
            core::track::Record& r = d.tracks->lib.rec[s.track];
            if (sp.crossed) {
                uint32_t best[core::lap::kSectors];
                for (int k = 0; k < core::lap::kSectors; ++k) best[k] = s.sectors.best_ms(k);
                if (core::track::offer_sectors(r, best)) s.track_dirty = true;
            }
            // The first clean lap placed the splits: they are the track's
            // from now on.
            if (!r.splits && s.sectors.placed()) {
                for (int k = 0; k < core::lap::kSectors - 1; ++k) {
                    r.split[k] = core::track::Line{s.sectors.line_at(k), s.sectors.heading_at(k)};
                }
                r.splits = true;
                s.track_dirty = true;
            }
        }
    } else if (silent && !s.lap_told_stale && s.last_lap_rx_ms != 0) {
        s.lap_told_stale = true;
        lf.valid = false;
        lf.t_ms = now;
        lu = s.lap.update(lf, lc);
        s.sectors.update(lf, g.course_deg, lu, lc);
    }

    // Peaks, every pass: this lap's and the session's.
    {
        const core::state::Attitude att = d.slots->attitude.get();
        const bool imu_ok = d.slots->health.get().ok(core::state::Dev::Imu);
        const float lean = imu_ok ? att.lean_deg : 0.0f;
        if (s.lap.state().phase == core::lap::Phase::Timing) {
            s.lap_peaks.add(m.best_mph, e.rpm, lean);
        }
        s.session_peaks.add(m.best_mph, e.rpm, lean);
    }
    s.lap.advance(now);
    const bool lap_event = lu.event != core::lap::Event::None;
    if (lu.event == core::lap::Event::Started) {
        if (d.delta) d.delta->start_lap();
        s.lap_peaks.reset();
    } else if (lu.event == core::lap::Event::Completed) {
        // The lap tag in the log is 0-based, so it is the count *before*
        // this lap is added.
        lap_done_index = s.lap_count;
        lap_done_ms = lu.lap_ms;
        s.last_done_ms = now;
        s.last_was_best = lu.valid && lu.lap_ms == s.lap.state().best_ms;
        if (s.lap_count < LogicState::kMaxLaps) s.laps_ms[s.lap_count++] = lu.lap_ms;
        if (d.lap_history && d.lap_history->count < LapHistory::kMax) {
            core::state::LapPeak& pk = d.lap_history->peak[d.lap_history->count];
            pk.mph = s.lap_peaks.mph;
            pk.rpm = s.lap_peaks.rpm;
            pk.lean_deg = s.lap_peaks.lean_deg();
            // The count last: render reads the history from the other core,
            // and a row must be whole before it is counted.
            d.lap_history->ms[d.lap_history->count] = lu.lap_ms;
            d.lap_history->best_ms = s.lap.state().best_ms;
            ++d.lap_history->count;
        }
        s.lap_peaks.reset();
        if (d.lap_trace) d.lap_trace->clear();      // trace the new lap
        if (d.delta) {
            // The reference is the quickest clean lap known: the session's,
            // or the saved track's until the session beats it. A lap with a
            // fix gap in it cannot be either, so a dropout cannot poison it.
            const bool is_ref = lu.valid && (s.ref_ms == 0 || lu.lap_ms < s.ref_ms);
            d.delta->finish_lap(is_ref);
            if (is_ref && d.delta->has_reference()) {
                s.ref_ms = lu.lap_ms;
                if (s.track >= 0) s.track_dirty = true;
            }
            d.delta->start_lap();
        }
        if (s.track >= 0 && d.tracks &&
            core::track::offer_lap(d.tracks->lib.rec[s.track], lu.lap_ms, lu.valid)) {
            s.track_dirty = true;
        }
    }

    int32_t delta_ms = 0;
    bool delta_valid = false;
    if (d.delta) delta_valid = d.delta->delta_ms(delta_ms);
    {
        core::state::Lap pl = core::state::to_published(s.lap.state(), delta_ms, delta_valid);
        pl.sector = static_cast<int8_t>(s.sectors.current());
        pl.split_sector = s.last_split.sector;
        pl.split_ms = s.last_split.crossed ? s.last_split.ms : 0;
        pl.split_delta_ms = s.last_split.delta_ms;
        pl.split_delta_valid = s.last_split.delta_valid;
        pl.split_at_ms = s.last_split.at_ms;
        pl.ideal_ms = s.sectors.ideal_ms();
        pl.last_done_ms = s.last_done_ms;
        pl.last_was_best = s.last_was_best;
        pl.max_lean_left_deg = s.session_peaks.lean_left_deg;
        pl.max_lean_right_deg = s.session_peaks.lean_right_deg;
        pl.track = s.track;
        pl.track_recalled = s.track_recalled;
        pl.line_refused_seq = s.line_refused_seq;
        if (s.track >= 0 && d.tracks) {
            const core::track::Record& r = d.tracks->lib.rec[s.track];
            pl.track_best_ms = r.best_ms;
            pl.track_ideal_ms = r.ideal_ms();
        }
        d.slots->lap.publish(pl);
    }

    // --- traces ---------------------------------------------------------
    // One point per fix, at most 10 Hz (the sim makes a fix every tick).
    // Spaced by the fixes' own times, not by this tick's: fixes 100 ms
    // apart land on a 20 ms tick grid, so two can reach here 80 ms apart,
    // and a gate on `now` dropped the second one -- a 200 ms hole in the
    // trace and the delta, for minutes at a time as the two clocks drifted.
    if (g.fix && new_fix && g.rx_ms - s.last_trace_ms >= 90) {
        s.last_trace_ms = g.rx_ms;
        ensure_origin(s, core::geo::LatLon{g.lat, g.lon});
        core::state::TrackPoint p;
        to_local_ft(s.origin, core::geo::LatLon{g.lat, g.lon}, p.x_ft, p.y_ft);
        p.speed_mph = m.best_mph;
        p.t_ms = now;
        if (d.trip_trace) d.trip_trace->push(p);
        if (d.lap_trace) d.lap_trace->push(p);
        if (d.delta && s.lap.state().phase == core::lap::Phase::Timing) {
            // Lap time at the fix, not at this pass: the position is the
            // fix's, and the pass runs a little after it (the gnss wake, or
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
        blackbox_row(d, s, now, g, m, e, att, fo, lap_done_ms, lap_done_index);
    }

    if (detail) detail_log(d, s, now, scheduled, g, m, e, att, health, fi, fo);
}

}  // namespace services
