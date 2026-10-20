#include "check.hpp"
#include "core/log/detail.hpp"
#include <cmath>
#include <cstring>

using namespace core::log;

namespace {
bool eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
bool contains(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}
// Fields in a CSV line, quotes respected.
int fields(const char* line) {
    int n = 1;
    bool q = false;
    for (const char* p = line; *p && *p != '\n'; ++p) {
        if (*p == '"') q = !q;
        else if (*p == ',' && !q) ++n;
    }
    return n;
}
// The header is the last line of what the header function gives.
const char* last_line(const char* s) {
    const char* last = s;
    for (const char* p = s; *p; ++p) {
        if (*p == '\n' && p[1]) last = p + 1;
    }
    return last;
}
}  // namespace

int main() {
    CASE("detail files are named after the session, plain and dated");
    {
        char b[64];
        CHECK(stream_file_name(b, sizeof b, Date{}, 7, kImu) > 0);
        CHECK(eq(b, "detail/s007_imu.csv"));
        Date d; d.year = 2026; d.month = 10; d.day = 13;
        stream_file_name(b, sizeof b, d, 42, kTach);
        CHECK(eq(b, "detail/2026-10-13_s042_tach.csv"));
        stream_file_name(b, sizeof b, d, 42, kDiag);
        CHECK(eq(b, "detail/2026-10-13_s042_diag.csv"));
        // kMain is the blackbox's own name, unchanged.
        char m[64];
        stream_file_name(b, sizeof b, d, 42, kMain);
        log_file_name(m, sizeof m, d, 42);
        CHECK(eq(b, m));
        // Too small a buffer is refused, not truncated into another name.
        char tiny[12];
        CHECK(stream_file_name(tiny, sizeof tiny, d, 42, kDet) == 0);
        CHECK(tiny[0] == '\0');
    }

    CASE("a detail file's session counts, so its number is not given out again");
    {
        CHECK(session_of("s007.csv") == 7);
        CHECK(session_of("2026-10-13_s042.csv") == 42);
        CHECK(session_of("s007_imu.csv") == 7);
        CHECK(session_of("2026-10-13_s042_diag.csv") == 42);
        CHECK(session_of("s999_tach.csv") == 999);
        CHECK(session_of("notes.csv") == -1);
        CHECK(session_of("xs007.csv") == -1);
        CHECK(session_of("xs007_imu.csv") == -1);
        CHECK(session_of("s007_IMU.csv") == -1);
        CHECK(session_of("s007_.csv") == -1);
        CHECK(session_of("s07_imu.csv") == -1);
        CHECK(session_of("_imu.csv") == -1);
        CHECK(session_of("s007_imu.txt") == -1);
        CHECK(session_of(nullptr) == -1);
    }

    CASE("pulse window: steady pulses give their interval and nothing odd");
    {
        PulseWindow w;
        uint64_t t = 1000000;
        uint32_t n = 100;
        w.update(n, t);          // primes
        for (int i = 0; i < 10; ++i) { t += 10000; w.update(++n, t); }
        const PulseStats p = w.take();
        CHECK(p.pulses == 10);
        CHECK(p.min_us == 10000);
        CHECK(p.max_us == 10000);
        CHECK(p.last_us == 10000);
        CHECK(p.short_n == 0);
        CHECK(p.long_n == 0);
        CHECK(p.multi == 0);
        CHECK(p.torn == 0);
        // take() starts a new window; the interval history carries on.
        const PulseStats e = w.take();
        CHECK(e.pulses == 0 && e.min_us == 0 && e.max_us == 0);
        t += 10000; w.update(++n, t);
        CHECK(w.peek().pulses == 1 && w.peek().short_n == 0);
    }

    CASE("pulse window: an extra edge reads short, a missing one long");
    {
        PulseWindow w;
        uint64_t t = 0;
        uint32_t n = 0;
        w.update(n, t);
        for (int i = 0; i < 5; ++i) { t += 10000; w.update(++n, t); }
        t += 3000;  w.update(++n, t);     // a second edge from one spark
        CHECK(w.peek().short_n == 1);
        CHECK(w.peek().min_us == 3000);
        t += 7000;  w.update(++n, t);     // back on the beat
        t += 10000; w.update(++n, t);
        t += 20000; w.update(++n, t);     // a spark that never arrived
        const PulseStats p = w.take();
        CHECK(p.long_n == 1);
        CHECK(p.max_us == 20000);
    }

    CASE("pulse window: several pulses in one read are averaged and flagged");
    {
        PulseWindow w;
        w.update(0, 0);
        w.update(3, 30000);
        const PulseStats p = w.take();
        CHECK(p.pulses == 3);
        CHECK(p.multi == 1);
        CHECK(p.min_us == 10000);
    }

    CASE("pulse window: a torn read is not spent, and the next good edge measures it");
    {
        PulseWindow w;
        w.update(10, 100000);
        w.update(11, 110000);
        w.update(12, 110000);             // count moved, time did not
        CHECK(w.peek().torn == 1);
        CHECK(w.peek().pulses == 1);
        w.update(12, 120000);             // the good read: one pulse, 10 ms
        CHECK(w.peek().pulses == 2);
        CHECK(w.peek().max_us == 10000);
    }

    CASE("pulse window: the first pulse after a stop is not an interval; the counter wraps");
    {
        PulseWindow w;
        w.update(0xFFFFFFFEu, 0);
        w.update(0xFFFFFFFFu, 10000);
        w.update(0x00000000u, 20000);     // wrapped
        CHECK(w.peek().pulses == 2);
        CHECK(w.peek().max_us == 10000);
        w.update(1u, 20000 + 5000000);    // five seconds later
        CHECK(w.peek().pulses == 3);
        CHECK(w.peek().max_us == 10000);  // not 5 s
        CHECK(w.peek().long_n == 0);
        w.update(2u, 20000 + 5000000 + 9000);
        CHECK(w.peek().long_n == 0 && w.peek().short_n == 0);
    }

    CASE("imu rows: exact time, sensor values, empty for not-a-number");
    {
        ImuRow r;
        r.t_ms = 7200123;   // two hours in: a float second would lose this ms
        r.ax_g = 0.0123f; r.ay_g = -0.5f; r.az_g = 1.0f;
        r.gx_dps = 1.5f; r.gy_dps = -0.25f; r.gz_dps = 0.0f;
        r.temp_c = 31.25f;
        r.batch_n = 16; r.drain = 2;
        r.chip_ts = 65535; r.fifo_hdr = 0x68;   // the top of the wrap, raw
        char b[160];
        format_imu(b, sizeof b, r);
        CHECK(eq(b, "7200.123,0.0123,-0.5000,1.0000,1.500,-0.250,0.000,31.3,16,2,65535,104\n") ||
              eq(b, "7200.123,0.0123,-0.5000,1.0000,1.500,-0.250,0.000,31.2,16,2,65535,104\n"));
        r.gx_dps = std::nanf("");
        format_imu(b, sizeof b, r);
        CHECK(contains(b, "1.0000,,-0.250"));
        r.t_ms = 5;
        format_imu(b, sizeof b, r);
        CHECK(std::strncmp(b, "0.005,", 6) == 0);
    }

    CASE("every row has as many fields as its header names");
    {
        char h[1024], b[1024];
        imu_header(h, sizeof h);
        format_imu(b, sizeof b, ImuRow{});
        CHECK(fields(last_line(h)) == fields(b));

        tach_header(h, sizeof h);
        TachRow t;
        t.tach.pulses = 3; t.tach_rejected = 12; t.tach_ring_us = 640;
        t.raw_rpm = 6012.4f; t.late = 2;
        format_tach(b, sizeof b, t);
        CHECK(fields(last_line(h)) == fields(b));
        CHECK(contains(b, ",12,640,6012,"));

        det_header(h, sizeof h);
        DetRow d;
        d.used = 0x0F; d.rejected = 0x02; d.suspect = 0;
        d.front_verdict = 2;
        format_det(b, sizeof b, d);
        CHECK(fields(last_line(h)) == fields(b));
        CHECK(contains(b, ",AFRG,F,-,"));
        CHECK(contains(b, ",Unknown,"));   // rear
        CHECK(contains(b, ",Dead,"));      // front

        diag_header(h, sizeof h);
        DiagRow g;
        std::strcpy(g.stack_low_task, "render");
        g.battery_verdict = 3;             // a name with a space in it
        g.dev_state[0] = static_cast<uint8_t>(core::state::DevState::Ok);
        format_diag(b, sizeof b, g);
        CHECK(fields(last_line(h)) == fields(b));
        CHECK(contains(h, "Rows_imu,Drop_imu"));
        CHECK(contains(h, "IMU,IMUFaults"));
        CHECK(contains(b, ",render,"));
        CHECK(contains(b, ",Ok,0,"));
    }

    CASE("a row that does not fit is cut, terminated, and says how long it would be");
    {
        char b[16];
        DetRow d;
        const size_t n = format_det(b, sizeof b, d);
        CHECK(n == sizeof b - 1);
        CHECK(b[sizeof b - 1] == '\0');
    }

    CASE("commands are logged with what they set");
    {
        const core::state::Command c = core::state::Command::config_set(0x0901, 3.0f);
        const EventRow e = command_event(1500, c);
        char b[160];
        format_event(b, sizeof b, e);
        CHECK(std::strncmp(b, "1.500,cmd,", 10) == 0);
        CHECK(contains(b, "key=0x0901"));
        CHECK(contains(b, "value=3.000"));
        const EventRow t = command_event(2000, core::state::Command(core::state::CommandId::TripReset, 1));
        format_event(b, sizeof b, t);
        CHECK(contains(b, "arg=1"));
        CHECK(!contains(b, "key="));
    }

    CASE("the watcher logs what is true at key-on once, then only changes");
    {
        EventWatcher ew;
        EventRow out[EventWatcher::kMaxPerPass];
        Watched w;
        w.health.dev[static_cast<int>(core::state::Dev::Imu)].state = core::state::DevState::Ok;
        w.health.dev[static_cast<int>(core::state::Dev::LedDriver)].state = core::state::DevState::Fault;
        w.health.dev[static_cast<int>(core::state::Dev::LedDriver)].fault_count = 3;
        w.health.dev[static_cast<int>(core::state::Dev::LedDriver)].last_err = -5;
        size_t n = ew.watch(w, 100, out, EventWatcher::kMaxPerPass);
        CHECK(n == 2);
        char b[160];
        format_event(b, sizeof b, out[1]);
        CHECK(contains(b, "health"));
        CHECK(contains(b, "dev=\"LED driver\""));
        CHECK(contains(b, "state=Fault"));
        CHECK(contains(b, "err=-5"));
        CHECK(contains(b, "faults=3"));
        CHECK(ew.watch(w, 120, out, EventWatcher::kMaxPerPass) == 0);

        w.fix = true; w.sats = 9; w.hdop = 0.9f;
        n = ew.watch(w, 140, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Fix && out[0].a == 1 && out[0].b == 9);
        w.fix = false;
        n = ew.watch(w, 160, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Fix && out[0].a == 0);
    }

    CASE("the watcher: engine stop keeps the last rpm; gear, lap, drop, trick");
    {
        EventWatcher ew;
        EventRow out[EventWatcher::kMaxPerPass];
        Watched w;
        ew.watch(w, 0, out, EventWatcher::kMaxPerPass);
        w.rpm = 1500.0f;
        size_t n = ew.watch(w, 20, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Engine && out[0].a == 1);
        w.rpm = 0.0f;
        n = ew.watch(w, 40, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Engine && out[0].a == 0);
        CHECK_NEAR(out[0].x, 1500.0f, 0.01);

        w.gear = 2; w.rpm = 5000.0f; w.mph = 20.0f;
        n = ew.watch(w, 60, out, EventWatcher::kMaxPerPass);
        CHECK(n == 2);   // engine running, and the gear
        bool gear = false;
        for (size_t i = 0; i < n; ++i) gear = gear || (out[i].kind == Ev::Gear && out[i].a == 2);
        CHECK(gear);

        w.last_done_ms = 9000; w.last_lap_ms = 63210; w.lap_count = 1; w.last_was_best = true;
        n = ew.watch(w, 80, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Lap && out[0].i == 63210 && out[0].a == 1);

        w.dropped[kImu] = 40;
        n = ew.watch(w, 100, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Drop && out[0].a == kImu && out[0].i == 40);
        char b[160];
        format_event(b, sizeof b, out[0]);
        CHECK(contains(b, "stream=imu dropped=40"));

        w.stunt_seq = 1; w.stunt_kind = 1; w.stunt_grade = 2; w.stunt_ms = 2300;
        w.stunt_peak_deg = 34.0f; w.stunt_points = 410;
        n = ew.watch(w, 120, out, EventWatcher::kMaxPerPass);
        CHECK(n == 1 && out[0].kind == Ev::Stunt);
        format_event(b, sizeof b, out[0]);
        CHECK(contains(b, "kind=wheelie grade=\"NICE ONE\" ms=2300"));
    }

    CASE("the watcher never writes past the caller's array");
    {
        EventWatcher ew;
        EventRow out[3];
        Watched w;
        for (uint8_t d = 0; d < core::state::kDevCount; ++d) {
            w.health.dev[d].state = core::state::DevState::Ok;
        }
        CHECK(ew.watch(w, 0, out, 3) == 3);
    }

    return check::report("detail");
}
