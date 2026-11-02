#include "core/log/detail.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/battery/battery.hpp"
#include "core/stunt/stunt.hpp"
#include "core/units/units.hpp"

namespace core::log {
namespace {

// A line into a caller's buffer, never past its end. Like log.cpp's put
// helpers, as a small object so a row reads as its columns.
class Out {
public:
    Out(char* buf, size_t n) : b_(buf), n_(n) {}

    void c(char ch) {
        if (k_ + 1 < n_) b_[k_] = ch;
        ++k_;
    }
    void s(const char* str) {
        for (; str && *str; ++str) c(*str);
    }
    void u(uint32_t v) {
        char t[12];
        int d = 0;
        do { t[d++] = static_cast<char>('0' + v % 10); v /= 10; } while (v > 0);
        while (d > 0) c(t[--d]);
    }
    void i(int32_t v) {
        if (v < 0) {
            c('-');
            u(static_cast<uint32_t>(-(static_cast<int64_t>(v))));
        } else {
            u(static_cast<uint32_t>(v));
        }
    }
    // Not a number is an empty field: pandas reads that as NaN, and a "--"
    // would turn the whole column into strings.
    void f(float v, int decimals) {
        if (!std::isfinite(v)) return;
        char t[24];
        const size_t len = units::fmt_float(t, sizeof t, v, decimals);
        for (size_t j = 0; j < len; ++j) c(t[j]);
    }
    void b(bool v) { c(v ? '1' : '0'); }
    // Milliseconds as seconds, exactly: a float second loses the millisecond
    // after a couple of hours, and at 200 Hz that is a sample's position.
    void time(uint32_t ms) {
        u(ms / 1000u);
        c('.');
        const uint32_t r = ms % 1000u;
        c(static_cast<char>('0' + r / 100));
        c(static_cast<char>('0' + (r / 10) % 10));
        c(static_cast<char>('0' + r % 10));
    }
    void hex(uint32_t v, int digits) {
        static const char kHex[] = "0123456789abcdef";
        s("0x");
        for (int d = digits - 1; d >= 0; --d) c(kHex[(v >> (4 * d)) & 0xF]);
    }
    void sep() { c(','); }

    // A key=value pair of an event's detail, space-separated after the first.
    void kv(const char* key) {
        if (pairs_++ > 0) c(' ');
        s(key);
        c('=');
    }
    // A value with a space in it, quoted for shlex.
    void quoted(const char* str) {
        bool space = false;
        for (const char* p = str; p && *p; ++p) space = space || *p == ' ';
        if (space) c('"');
        s(str);
        if (space) c('"');
    }

    size_t done() {
        if (n_ == 0) return 0;
        const size_t w = k_ < n_ - 1 ? k_ : n_ - 1;
        b_[w] = '\0';
        return w;
    }

private:
    char*  b_;
    size_t n_;
    size_t k_ = 0;
    int    pairs_ = 0;
};

size_t literal(char* buf, size_t n, const char* text) {
    Out o(buf, n);
    o.s(text);
    return o.done();
}

void pulse_cols(Out& o, const PulseStats& p) {
    o.u(p.pulses);   o.sep();
    o.u(p.min_us);   o.sep();
    o.u(p.max_us);   o.sep();
    o.u(p.last_us);  o.sep();
    o.u(p.short_n);  o.sep();
    o.u(p.long_n);   o.sep();
    o.u(p.multi);    o.sep();
    o.u(p.torn);
}

// Short names for the diag columns, in core::state::Dev order. dev_name()
// has spaces, which a column name should not.
constexpr const char* kDevCol[state::kDevCount] = {
    "IMU", "GPS", "LED", "Pod", "SD", "ECU", "Display", "BLE", "Phone",
    "TPMSF", "TPMSR", "FrontWheel", "RearWheel",
};

const char* dev_state_name(uint8_t s) {
    switch (static_cast<state::DevState>(s)) {
        case state::DevState::Absent:   return "Absent";
        case state::DevState::Init:     return "Init";
        case state::DevState::Ok:       return "Ok";
        case state::DevState::Degraded: return "Degraded";
        case state::DevState::Fault:    return "Fault";
    }
    return "?";
}

const char* wheel_name(uint8_t w) {
    switch (w) {
        case 0: return "Unknown";
        case 1: return "Ok";
        case 2: return "Dead";
    }
    return "?";
}

const char* lap_phase_name(uint8_t p) {
    switch (p) {
        case 0: return "NoLine";
        case 1: return "Armed";
        case 2: return "Timing";
    }
    return "?";
}

const char* gearcal_phase_name(uint8_t p) {
    switch (p) {
        case 0: return "Idle";
        case 1: return "Intro";
        case 2: return "Measuring";
        case 3: return "Summary";
    }
    return "?";
}

// core::state::Perf::Metric order.
const char* perf_name(uint8_t m) {
    static const char* const kNames[] = {
        "to30", "to60", "60ft", "330ft", "eighth", "brake30", "brake60",
    };
    static_assert(sizeof kNames / sizeof kNames[0] == state::Perf::kCount,
                  "a perf metric without a name");
    return m < state::Perf::kCount ? kNames[m] : "?";
}

const char* stunt_kind_name(uint8_t k) {
    switch (k) {
        case 1: return "wheelie";
        case 2: return "stoppie";
        case 3: return "loopout";
    }
    return "none";
}

// core::fusion::Source bits as letters: A accel, F front, R rear, G GPS.
void source_bits(Out& o, uint8_t bits) {
    if (bits == 0) { o.c('-'); return; }
    if (bits & 1u) o.c('A');
    if (bits & 2u) o.c('F');
    if (bits & 4u) o.c('R');
    if (bits & 8u) o.c('G');
}

const char* ev_name(Ev e) {
    switch (e) {
        case Ev::None:        return "none";
        case Ev::Command:     return "cmd";
        case Ev::Health:      return "health";
        case Ev::Fix:         return "fix";
        case Ev::Lap:         return "lap";
        case Ev::Split:       return "split";
        case Ev::LapPhase:    return "lap_phase";
        case Ev::TakeOff:     return "takeoff";
        case Ev::Land:        return "land";
        case Ev::Slide:       return "slide";
        case Ev::Stunt:       return "stunt";
        case Ev::Perf:        return "perf";
        case Ev::Wheel:       return "wheel";
        case Ev::PowerFail:   return "power_fail";
        case Ev::Battery:     return "battery";
        case Ev::Gear:        return "gear";
        case Ev::Engine:      return "engine";
        case Ev::Moving:      return "moving";
        case Ev::GearCal:     return "gearcal";
        case Ev::FusionReset: return "fusion_reset";
        case Ev::Suspect:     return "suspect";
        case Ev::Drop:        return "drop";
        case Ev::Heart:       return "heart";
        case Ev::HrRecovery:  return "hr_recovery";
    }
    return "?";
}

bool same_name_end(const char* name, size_t end) {
    // `name[end-4 .. end)` is sNNN, at the start or after the date's '_'.
    if (end < 4) return false;
    const char* s = name + end - 4;
    if (s[0] != 's') return false;
    if (s != name && s[-1] != '_') return false;
    for (int i = 1; i <= 3; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

}  // namespace

// --- names -----------------------------------------------------------------------

const char* stream_suffix(Stream s) {
    switch (s) {
        case kMain: return "";
        case kImu:  return "imu";
        case kTach: return "tach";
        case kDet:  return "det";
        case kEvt:  return "evt";
        case kDiag: return "diag";
        case kStreamCount: break;
    }
    return "";
}

size_t stream_file_name(char* buf, size_t n, Date date, unsigned session, Stream s) {
    if (s == kMain) return log_file_name(buf, n, date, session);
    if (!buf || n == 0) return 0;
    session %= 1000;
    int len;
    if (date.year == 0) {
        len = std::snprintf(buf, n, "%s/s%03u_%s.csv", kDetailDir, session,
                            stream_suffix(s));
    } else {
        len = std::snprintf(buf, n, "%s/%04u-%02u-%02u_s%03u_%s.csv", kDetailDir,
                            static_cast<unsigned>(date.year),
                            static_cast<unsigned>(date.month),
                            static_cast<unsigned>(date.day), session,
                            stream_suffix(s));
    }
    if (len < 0 || static_cast<size_t>(len) >= n) { buf[0] = '\0'; return 0; }
    return static_cast<size_t>(len);
}

// Declared in log.hpp. Here because it now knows the detail files' names: a
// session whose blackbox was deleted from the Wi-Fi page but whose detail
// files are still on the card must not have its number given out again, or
// the next ride's rows are appended to the old ride's files.
int session_of(const char* name) {
    if (!name) return -1;
    const size_t n = std::strlen(name);
    if (n < 8 || std::strcmp(name + n - 4, ".csv") != 0) return -1;
    size_t end = n - 4;
    // An optional _suffix of lowercase letters after the number.
    size_t j = end;
    while (j > 0 && name[j - 1] >= 'a' && name[j - 1] <= 'z') --j;
    if (j < end && j >= 2 && name[j - 1] == '_' && name[j - 2] >= '0' &&
        name[j - 2] <= '9') {
        end = j - 1;
    }
    if (!same_name_end(name, end)) return -1;
    const char* s = name + end - 4;
    return (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

// --- imu -------------------------------------------------------------------------

size_t imu_header(char* buf, size_t n) {
    return literal(buf, n,
        "Time,Ax,Ay,Az,Gx,Gy,Gz,TempC,Batch,Drain,ChipTs,Hdr\n");
}

size_t format_imu(char* buf, size_t n, const ImuRow& r) {
    Out o(buf, n);
    o.time(r.t_ms);    o.sep();
    o.f(r.ax_g, 4);    o.sep();
    o.f(r.ay_g, 4);    o.sep();
    o.f(r.az_g, 4);    o.sep();
    o.f(r.gx_dps, 3);  o.sep();
    o.f(r.gy_dps, 3);  o.sep();
    o.f(r.gz_dps, 3);  o.sep();
    o.f(r.temp_c, 1);  o.sep();
    o.u(r.batch_n);    o.sep();
    o.u(r.drain);      o.sep();
    o.u(r.chip_ts);    o.sep();
    o.u(r.fifo_hdr);
    o.c('\n');
    return o.done();
}

// --- pulses ----------------------------------------------------------------------

void PulseWindow::update(uint32_t total, uint64_t edge_us) {
    if (!started_) {
        started_ = true;
        last_total_ = total;
        last_edge_us_ = edge_us;
        return;
    }
    const uint32_t dp = total - last_total_;      // wraps cleanly
    if (dp == 0) return;
    if (edge_us <= last_edge_us_) {
        // Counted with no newer time: left in the counter, as core::tach
        // does, and measured with the next good edge.
        ++w_.torn;
        return;
    }
    const uint64_t span = edge_us - last_edge_us_;
    last_total_ = total;
    last_edge_us_ = edge_us;
    w_.pulses = static_cast<uint16_t>(w_.pulses + dp);
    if (dp > 1) ++w_.multi;

    const uint64_t per = span / dp;
    if (per > kMaxIntervalUs) {
        // The first pulse after a stop: no interval worth comparing.
        prev_us_ = 0;
        return;
    }
    const uint32_t us = static_cast<uint32_t>(per);
    bool odd = false;
    if (prev_us_ != 0) {
        if (us * 2u < prev_us_) {
            ++w_.short_n;
            odd = true;
        } else if (static_cast<uint64_t>(us) * 10u > static_cast<uint64_t>(prev_us_) * 18u) {
            ++w_.long_n;
            odd = true;
        }
    }
    // An odd interval moves the reference only half way. An extra edge
    // splits one interval in two, and against the short half the rest of it
    // read long: one glitch counted twice. A real change of pace (a wheel
    // locking) still gets there in a few pulses.
    prev_us_ = odd ? (prev_us_ + us) / 2u : us;
    if (w_.min_us == 0 || us < w_.min_us) w_.min_us = us;
    if (us > w_.max_us) w_.max_us = us;
    w_.last_us = us;
}

PulseStats PulseWindow::take() {
    const PulseStats out = w_;
    w_ = PulseStats{};
    return out;
}

// --- tach ------------------------------------------------------------------------

size_t tach_header(char* buf, size_t n) {
    return literal(buf, n,
        "Time,"
        "TachPulses,TachMinUs,TachMaxUs,TachLastUs,TachShort,TachLong,TachMulti,TachTorn,"
        "TachRejected,TachRingUs,"
        "RawRPM,RPM,RPMRate,LampRPM,Mask,Neutral,SenseOk,"
        "FrontPulses,FrontMinUs,FrontMaxUs,FrontLastUs,FrontShort,FrontLong,FrontMulti,FrontTorn,"
        "FrontRejected,FrontMPH,"
        "RearPulses,RearMinUs,RearMaxUs,RearLastUs,RearShort,RearLong,RearMulti,RearTorn,"
        "RearRejected,RearMPH,"
        "EdgeWakes,WakeLatMaxUs,LampLatMaxUs,WorkMaxUs,LampWrites,LampErrors,Late\n");
}

size_t format_tach(char* buf, size_t n, const TachRow& r) {
    Out o(buf, n);
    o.time(r.t_ms);          o.sep();
    pulse_cols(o, r.tach);   o.sep();
    o.u(r.tach_rejected);    o.sep();
    o.u(r.tach_ring_us);     o.sep();
    o.f(r.raw_rpm, 0);       o.sep();
    o.f(r.rpm, 0);           o.sep();
    o.f(r.rpm_rate, 0);      o.sep();
    o.u(r.lamp_rpm);         o.sep();
    o.u(r.mask);             o.sep();
    o.b(r.neutral);          o.sep();
    o.b(r.sense_ok);         o.sep();
    pulse_cols(o, r.front);  o.sep();
    o.u(r.front_rejected);   o.sep();
    o.f(r.front_mph, 2);     o.sep();
    pulse_cols(o, r.rear);   o.sep();
    o.u(r.rear_rejected);    o.sep();
    o.f(r.rear_mph, 2);      o.sep();
    o.u(r.edge_wakes);       o.sep();
    o.u(r.wake_lat_max_us);  o.sep();
    o.u(r.lamp_lat_max_us);  o.sep();
    o.u(r.work_max_us);      o.sep();
    o.u(r.lamp_writes);      o.sep();
    o.u(r.lamp_errors);      o.sep();
    o.u(r.late);
    o.c('\n');
    return o.done();
}

// --- det -------------------------------------------------------------------------

size_t det_header(char* buf, size_t n) {
    return literal(buf, n,
        "Time,ShownMPH,FrontMPH,RearMPH,GpsMPH,HDOP,GpsNew,"
        "FusedMPH,FusedSigma,FusedValid,Used,Rejected,Suspect,Resets,AccelG,AccelBiasG,"
        "FrontVerdict,RearVerdict,"
        "Lean,AccelLean,Pitch,Steer,YawRate,SlideRate,Heading,Sliding,Airborne,"
        "GLat,GLon,GVert,BiasGx,BiasGy,BiasGz,"
        "GroundDeg,LiftDeg,StuntKind,"
        "RPM,Ratio,Gear,Neutral,ShiftMask,"
        "LapPhase,DistToLineFt,PerfPhase,GearCalPhase,VBat,Still\n");
}

size_t format_det(char* buf, size_t n, const DetRow& r) {
    Out o(buf, n);
    o.time(r.t_ms);           o.sep();
    o.f(r.shown_mph, 2);      o.sep();
    o.f(r.front_mph, 2);      o.sep();
    o.f(r.rear_mph, 2);       o.sep();
    o.f(r.gps_mph, 2);        o.sep();
    o.f(r.hdop, 1);           o.sep();
    o.b(r.gps_new);           o.sep();
    o.f(r.fused_mph, 2);      o.sep();
    o.f(r.fused_sigma, 2);    o.sep();
    o.b(r.fused_valid);       o.sep();
    source_bits(o, r.used);   o.sep();
    source_bits(o, r.rejected); o.sep();
    source_bits(o, r.suspect);  o.sep();
    o.u(r.resets);            o.sep();
    o.f(r.accel_g, 3);        o.sep();
    o.f(r.accel_bias_g, 4);   o.sep();
    o.s(wheel_name(r.front_verdict)); o.sep();
    o.s(wheel_name(r.rear_verdict));  o.sep();
    o.f(r.lean_deg, 2);       o.sep();
    o.f(r.accel_lean_deg, 2); o.sep();
    o.f(r.pitch_deg, 2);      o.sep();
    o.f(r.steer_deg, 2);      o.sep();
    o.f(r.yaw_rate_dps, 2);   o.sep();
    o.f(r.slide_rate_dps, 1); o.sep();
    o.f(r.heading_deg, 1);    o.sep();
    o.b(r.sliding);           o.sep();
    o.b(r.airborne);          o.sep();
    o.f(r.g_lat, 3);          o.sep();
    o.f(r.g_lon, 3);          o.sep();
    o.f(r.g_vert, 3);         o.sep();
    o.f(r.bias_gx, 3);        o.sep();
    o.f(r.bias_gy, 3);        o.sep();
    o.f(r.bias_gz, 3);        o.sep();
    o.f(r.ground_deg, 2);     o.sep();
    o.f(r.lift_deg, 2);       o.sep();
    o.s(stunt_kind_name(r.stunt_kind)); o.sep();
    o.f(r.rpm, 0);            o.sep();
    o.f(r.ratio, 1);          o.sep();
    o.u(r.gear);              o.sep();
    o.b(r.neutral);           o.sep();
    o.u(r.shift_mask);        o.sep();
    o.s(lap_phase_name(r.lap_phase)); o.sep();
    o.f(r.dist_to_line_ft, 0); o.sep();
    o.u(r.perf_phase);        o.sep();
    o.s(gearcal_phase_name(r.gearcal_phase)); o.sep();
    o.f(r.vbat, 2);           o.sep();
    o.b(r.still);
    o.c('\n');
    return o.done();
}

// --- events ----------------------------------------------------------------------

EventRow command_event(uint32_t t_ms, const state::Command& c) {
    EventRow e;
    e.t_ms = t_ms;
    e.kind = Ev::Command;
    e.a = static_cast<uint8_t>(c.id);
    if (c.id == state::CommandId::ConfigSet) {
        e.i = c.cfg.key;
        e.x = c.cfg.val;
    } else {
        e.i = c.i;
    }
    return e;
}

size_t event_header(char* buf, size_t n) {
    return literal(buf, n, "Time,Event,Detail\n");
}

size_t format_event(char* buf, size_t n, const EventRow& e) {
    Out o(buf, n);
    o.time(e.t_ms);
    o.sep();
    o.s(ev_name(e.kind));
    o.sep();
    switch (e.kind) {
        case Ev::Command: {
            const auto id = static_cast<state::CommandId>(e.a);
            o.kv("name"); o.s(state::command_name(id));
            if (id == state::CommandId::ConfigSet) {
                o.kv("key"); o.hex(static_cast<uint32_t>(e.i), 4);
                o.kv("value"); o.f(e.x, 3);
            } else {
                o.kv("arg"); o.i(e.i);
            }
            break;
        }
        case Ev::Health:
            o.kv("dev");
            o.quoted(e.a < state::kDevCount
                         ? state::dev_name(static_cast<state::Dev>(e.a)) : "?");
            o.kv("state"); o.s(dev_state_name(static_cast<uint8_t>(e.b)));
            o.kv("err"); o.i(e.i);
            o.kv("faults"); o.f(e.x, 0);
            break;
        case Ev::Fix:
            o.kv("fix"); o.u(e.a);
            o.kv("sats"); o.u(e.b);
            o.kv("hdop"); o.f(e.x, 1);
            break;
        case Ev::Lap:
            o.kv("laps"); o.u(e.b);
            o.kv("ms"); o.i(e.i);
            o.kv("best"); o.u(e.a);
            break;
        case Ev::Split:
            o.kv("sector"); o.u(e.a);
            o.kv("ms"); o.i(e.i);
            break;
        case Ev::LapPhase:
            o.kv("phase"); o.s(lap_phase_name(e.a));
            break;
        case Ev::TakeOff:
            break;
        case Ev::Land:
            o.kv("air_ms"); o.i(e.i);
            o.kv("g"); o.f(e.x, 2);
            break;
        case Ev::Slide:
            o.kv("on"); o.u(e.a);
            o.kv("rate_dps"); o.f(e.x, 1);
            break;
        case Ev::Stunt:
            o.kv("kind"); o.s(stunt_kind_name(e.a));
            o.kv("grade"); o.quoted(stunt::grade_name(e.a, static_cast<uint8_t>(e.b)));
            o.kv("ms"); o.i(e.i);
            o.kv("peak_deg"); o.f(e.x, 1);
            o.kv("points"); o.f(e.y, 0);
            o.kv("ft"); o.f(e.z, 0);
            break;
        case Ev::Perf:
            o.kv("run"); o.s(perf_name(e.a));
            o.kv("best"); o.u(e.b);
            o.kv("ms"); o.i(e.i);
            o.kv("x"); o.f(e.x, 1);
            break;
        case Ev::Wheel:
            o.kv("wheel"); o.s(e.a == 0 ? "front" : "rear");
            o.kv("verdict"); o.s(wheel_name(static_cast<uint8_t>(e.b)));
            break;
        case Ev::PowerFail:
            o.kv("cut"); o.u(e.a);
            break;
        case Ev::Battery:
            o.kv("verdict");
            o.quoted(battery::verdict_name(static_cast<battery::Verdict>(e.a)));
            o.kv("rest_v"); o.f(e.x, 2);
            o.kv("crank_v"); o.f(e.y, 2);
            break;
        case Ev::Gear:
            o.kv("gear"); o.u(e.a);
            o.kv("rpm"); o.f(e.x, 0);
            o.kv("mph"); o.f(e.y, 1);
            break;
        case Ev::Engine:
            o.kv("running"); o.u(e.a);
            o.kv("rpm"); o.f(e.x, 0);
            break;
        case Ev::Moving:
            o.kv("moving"); o.u(e.a);
            break;
        case Ev::GearCal:
            o.kv("phase"); o.s(gearcal_phase_name(e.a));
            break;
        case Ev::FusionReset:
            o.kv("resets"); o.i(e.i);
            break;
        case Ev::Suspect:
            o.kv("sources"); source_bits(o, e.a);
            break;
        case Ev::Drop:
            o.kv("stream");
            o.s(e.a == kMain ? "main" : stream_suffix(static_cast<Stream>(e.a)));
            o.kv("dropped"); o.i(e.i);
            break;
        case Ev::Heart:
            o.kv("link"); o.s(e.a == state::Heart::Live ? "live"
                             : e.a == state::Heart::Connecting ? "connecting"
                             : e.a == state::Heart::Searching ? "searching" : "off");
            o.kv("bpm"); o.u(e.b);
            if (e.i) { o.kv("pulse"); o.s("none"); }
            break;
        case Ev::HrRecovery:
            o.kv("from"); o.u(e.b);
            o.kv("drop_1min"); o.i(e.i);
            if (e.a) { o.kv("drop_2min"); o.i(static_cast<int32_t>(e.x)); }
            break;
        case Ev::None:
            break;
    }
    o.c('\n');
    return o.done();
}

size_t EventWatcher::watch(const Watched& w, uint32_t t, EventRow* out, size_t max) {
    size_t k = 0;
    auto emit = [&](Ev kind) -> EventRow* {
        if (k >= max) return nullptr;
        EventRow& e = out[k++];
        e = EventRow{};
        e.t_ms = t;
        e.kind = kind;
        return &e;
    };
    const Watched& p = prev_;

    for (uint8_t d = 0; d < state::kDevCount; ++d) {
        const state::DevHealth& now = w.health.dev[d];
        const state::DevHealth& was = p.health.dev[d];
        const bool more_faults =
            now.fault_count != logged_faults_[d] &&
            static_cast<int32_t>(t - faults_next_ms_[d]) >= 0;
        if (now.state != was.state || more_faults) {
            logged_faults_[d] = now.fault_count;
            faults_next_ms_[d] = t + kFaultCountEveryMs;
            if (EventRow* e = emit(Ev::Health)) {
                e->a = d;
                e->b = static_cast<uint16_t>(now.state);
                e->i = now.last_err;
                e->x = static_cast<float>(now.fault_count);
            }
        }
    }
    if (w.fix != p.fix) {
        if (EventRow* e = emit(Ev::Fix)) {
            e->a = w.fix ? 1 : 0;
            e->b = w.sats;
            e->x = w.hdop;
        }
    }
    if (w.power_fail != p.power_fail) {
        if (EventRow* e = emit(Ev::PowerFail)) e->a = w.power_fail ? 1 : 0;
    }
    if (w.lap_phase != p.lap_phase) {
        if (EventRow* e = emit(Ev::LapPhase)) e->a = w.lap_phase;
    }
    if (w.last_done_ms != p.last_done_ms && w.last_done_ms != 0) {
        if (EventRow* e = emit(Ev::Lap)) {
            e->b = w.lap_count;
            e->i = static_cast<int32_t>(w.last_lap_ms);
            e->a = w.last_was_best ? 1 : 0;
        }
    }
    if (w.split_at_ms != p.split_at_ms && w.split_at_ms != 0) {
        if (EventRow* e = emit(Ev::Split)) {
            e->a = w.split_sector;
            e->i = static_cast<int32_t>(w.split_ms);
        }
    }
    if (w.airborne && !p.airborne) emit(Ev::TakeOff);
    if (w.land_ms != p.land_ms && w.land_ms != 0) {
        if (EventRow* e = emit(Ev::Land)) {
            e->i = static_cast<int32_t>(w.airtime_ms);
            e->x = w.land_g;
        }
    }
    if (w.sliding != p.sliding) {
        if (EventRow* e = emit(Ev::Slide)) {
            e->a = w.sliding ? 1 : 0;
            e->x = w.slide_rate_dps;
        }
    }
    if (w.stunt_seq != p.stunt_seq) {
        if (EventRow* e = emit(Ev::Stunt)) {
            e->a = w.stunt_kind;
            e->b = w.stunt_grade;
            e->i = static_cast<int32_t>(w.stunt_ms);
            e->x = w.stunt_peak_deg;
            e->y = static_cast<float>(w.stunt_points);
            e->z = w.stunt_ft;
        }
    }
    if (w.perf_seq != p.perf_seq) {
        if (EventRow* e = emit(Ev::Perf)) {
            e->a = w.perf_event;
            e->b = w.perf_best ? 1 : 0;
            e->i = static_cast<int32_t>(w.perf_ms);
            e->x = w.perf_x;
        }
    }
    if (w.front_verdict != p.front_verdict) {
        if (EventRow* e = emit(Ev::Wheel)) { e->a = 0; e->b = w.front_verdict; }
    }
    if (w.rear_verdict != p.rear_verdict) {
        if (EventRow* e = emit(Ev::Wheel)) { e->a = 1; e->b = w.rear_verdict; }
    }
    if (w.battery_verdict != p.battery_verdict) {
        if (EventRow* e = emit(Ev::Battery)) {
            e->a = w.battery_verdict;
            e->x = w.rest_v;
            e->y = w.crank_v;
        }
    }
    if (w.gear != p.gear) {
        if (EventRow* e = emit(Ev::Gear)) {
            e->a = w.gear;
            e->x = w.rpm;
            e->y = w.mph;
        }
    }
    if ((w.rpm > 0.0f) != (p.rpm > 0.0f)) {
        // The rpm at the edge is 0 when it stops, so the last running one
        // is the useful figure: what it died from.
        if (EventRow* e = emit(Ev::Engine)) {
            e->a = w.rpm > 0.0f ? 1 : 0;
            e->x = w.rpm > 0.0f ? w.rpm : p.rpm;
        }
    }
    if (w.moving != p.moving) {
        if (EventRow* e = emit(Ev::Moving)) e->a = w.moving ? 1 : 0;
    }
    if (w.gearcal_phase != p.gearcal_phase) {
        if (EventRow* e = emit(Ev::GearCal)) e->a = w.gearcal_phase;
    }
    if (w.fusion_resets != p.fusion_resets) {
        if (EventRow* e = emit(Ev::FusionReset)) e->i = w.fusion_resets;
    }
    if (w.suspect != p.suspect) {
        if (EventRow* e = emit(Ev::Suspect)) e->a = w.suspect;
    }
    for (uint8_t s = 0; s < kStreamCount; ++s) {
        if (w.dropped[s] != p.dropped[s]) {
            if (EventRow* e = emit(Ev::Drop)) {
                e->a = s;
                e->i = static_cast<int32_t>(w.dropped[s]);
            }
        }
    }

    if (w.hrr_seq != p.hrr_seq) {
        if (EventRow* e = emit(Ev::HrRecovery)) {
            e->a = w.hrr_has2 ? 1 : 0;
            e->b = w.hrr_from_bpm;
            e->i = w.hrr_drop1;
            e->x = static_cast<float>(w.hrr_drop2);
        }
    }
    if (w.hr_link != p.hr_link || w.hr_no_pulse != p.hr_no_pulse) {
        if (EventRow* e = emit(Ev::Heart)) {
            e->a = w.hr_link;
            e->b = w.hr_bpm;
            e->i = w.hr_no_pulse ? 1 : 0;
        }
    }

    prev_ = w;
    return k;
}

// --- diag ------------------------------------------------------------------------

size_t diag_header(char* buf, size_t n) {
    Out o(buf, n);
    o.s("Time,HeapFree,HeapMin,PsramFree,StackLowTask,StackLowFree,CtrlLate,RenderHz,"
        "ImuTimeouts,GnssSentences,GnssBadCsum,PodRecoveries,PodStuck,"
        "ChipTempC,ImuTempC,VBat,RestV,CrankV,ChargeV,Battery,"
        "Fix,Sats,Quality,HDOP,OdoMi,EngHours,FuelGal,");
    for (uint8_t s = 0; s < kStreamCount; ++s) {
        o.s("Rows_"); o.s(s == kMain ? "main" : stream_suffix(static_cast<Stream>(s))); o.sep();
        o.s("Drop_"); o.s(s == kMain ? "main" : stream_suffix(static_cast<Stream>(s))); o.sep();
    }
    o.s("SdBytes,SdMaxMs,SdSlow,SdErrors,");
    for (uint8_t d = 0; d < state::kDevCount; ++d) {
        o.s(kDevCol[d]); o.sep();
        o.s(kDevCol[d]); o.s("Faults"); o.sep();
    }
    o.s("RssiDbm,Wifi\n");
    return o.done();
}

size_t format_diag(char* buf, size_t n, const DiagRow& r) {
    Out o(buf, n);
    o.time(r.t_ms);              o.sep();
    o.u(r.heap_free_b);          o.sep();
    o.u(r.heap_min_b);           o.sep();
    o.u(r.psram_free_b);         o.sep();
    // A task name is ours and has no comma, but the column is text.
    for (size_t k = 0; k < sizeof r.stack_low_task && r.stack_low_task[k]; ++k) {
        const char ch = r.stack_low_task[k];
        o.c(ch == ',' ? '_' : ch);
    }
    o.sep();
    o.u(r.stack_low_free_b);     o.sep();
    o.u(r.ctrl_late);            o.sep();
    o.f(static_cast<float>(r.render_hz_x10) / 10.0f, 1); o.sep();
    o.u(r.imu_timeouts);         o.sep();
    o.u(r.gnss_sentences);       o.sep();
    o.u(r.gnss_bad_csum);        o.sep();
    o.u(r.pod_recoveries);       o.sep();
    o.u(r.pod_stuck);            o.sep();
    if (r.chip_temp_ok) o.f(r.chip_temp_c, 1);
    o.sep();
    o.f(r.imu_temp_c, 1);        o.sep();
    o.f(r.vbat, 2);              o.sep();
    o.f(r.rest_v, 2);            o.sep();
    o.f(r.crank_v, 2);           o.sep();
    o.f(r.charge_v, 2);          o.sep();
    o.quoted(battery::verdict_name(static_cast<battery::Verdict>(r.battery_verdict)));
    o.sep();
    o.b(r.fix);                  o.sep();
    o.u(r.sats);                 o.sep();
    o.u(r.quality);              o.sep();
    o.f(r.hdop, 1);              o.sep();
    o.f(r.odo_mi, 2);            o.sep();
    o.f(r.eng_hours, 3);         o.sep();
    o.f(r.fuel_used_gal, 3);     o.sep();
    for (uint8_t s = 0; s < kStreamCount; ++s) {
        o.u(r.rows[s]);    o.sep();
        o.u(r.dropped[s]); o.sep();
    }
    o.u(r.sd_bytes);             o.sep();
    o.u(r.sd_max_ms);            o.sep();
    o.u(r.sd_slow);              o.sep();
    o.u(r.sd_errors);            o.sep();
    for (uint8_t d = 0; d < state::kDevCount; ++d) {
        o.s(dev_state_name(r.dev_state[d])); o.sep();
        o.u(r.dev_faults[d]);                o.sep();
    }
    o.i(r.rssi_dbm);             o.sep();
    o.u(r.wifi_state);
    o.c('\n');
    return o.done();
}

}  // namespace core::log
