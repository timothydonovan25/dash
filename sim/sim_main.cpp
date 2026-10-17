// The dashboard, on a laptop.
//
// Runs the firmware's own UI and logic against a fake bike: the real
// ui::App, the real screens and widgets, the real services::logic_tick, the
// real core::input debounce, and the real core::shift::pattern() driving the
// lamps. The only thing that is not firmware is sim::Bike.
//
// Talks to a front end over stdin/stdout so the window can be anything. The
// protocol is deliberately plain text plus a PPM, which means you can also
// drive it from a shell script and pipe the frames into ffmpeg.
//
//   in   set <name> <value>      speed, rpm, lean, pitch, tps, coolant,
//                                vbat, sats, neutral, ecu, fix, auto,
//                                fslip, rslip (percent against road),
//                                deadf, deadr (that wheel sensor reads 0),
//                                down (on its side), temp (inside the dash, C),
//                                shift (hours added to the host clock),
//                                phone (a bonded phone connected),
//                                wclient (a phone using the Wi-Fi page),
//                                duration (media track length, ms),
//                                position (ms into the track, now),
//                                playstate (0 stop 1 play 2 pause),
//                                bus_mhz (panel write clock, 0 = instant)
//        btn <A-E> down|up       real debounce and long-press timing
//        cmd <lapline|laparm|lapclear|tripreset|jump|spin|flock|rlock|crank>
//        dev <imu|gnss|lamps|pod|sd|display> <ok|fail|absent>
//                                a device working, failing every report, or
//                                not there (for sd: the card pulled)
//        media <title>\t<album>\t<artist>\t<lyric>    lyric is optional;
//                                                     bare `media` clears it
//        quit
//
//   out  FRAME <lamps> <flash> <mode> <screen> <lapfrac> key=value...\n
//        (slip=KIND/pct, alert=TEXT, spaces as _, `-` for none;
//        bl=backlight percent, which the front end dims the picture by)
//        P6\n320 480\n255\n<pixels>
#include "sim_bike.hpp"
#include "panel_sim.hpp"

#include "core/alert/alert.hpp"
#include "core/config/config.hpp"
#include "core/health/health.hpp"
#include "core/input/input.hpp"
#include "core/gear/gear.hpp"
#include "core/shift/shift.hpp"
#include "core/tach/tach.hpp"
#include "core/slip/slip.hpp"
#include "core/sun/sun.hpp"
#include "core/wifi/wifi.hpp"
#include "core/sweep/sweep.hpp"
#include "services/logic_tick.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/wire_cost.hpp"
#include "ui/shadow.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <deque>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kTickMs  = 10;    // 100 Hz, the pod poll rate
constexpr uint32_t kFrameMs = 20;    // 50 Hz to the window; 100 Hz saturates the Tk front end

// The panel budget, modelled rather than drawn. The window here is a laptop
// and will happily repaint everything at any rate asked of it, which is
// exactly the thing that would let a screen quietly become unaffordable on
// the bike. So every frame is also priced: same band geometry as
// render_service, same strip split, same bus limit as bsp/buses.cpp.
constexpr int16_t kBandLines  = 40;
constexpr size_t  kBandPixels = static_cast<size_t>(ui::kScreenW) * kBandLines;
constexpr uint32_t kBusLimitBytes = 480u * 40u * 2u;   // the 8080 bus, RGB565

class DequeSource final : public services::ICommandSource,
                          public ui::ICommandSink {
public:
    bool post(const core::state::Command& c) override {
        q_.push_back(c);
        return true;
    }
    bool take(core::state::Command& out) override {
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::deque<core::state::Command> q_;
};

// Reads whole lines from stdin without blocking the tick loop.
//
// It has to be non-blocking: a blocking read here stalls the simulated bike
// whenever the front end has nothing to say, which is most of the time, and
// the dash freezes between slider movements.
bool poll_line(std::string& out) {
    static std::string buf;
    static bool eof = false;

    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            out = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            return true;
        }
        if (eof) return false;

        pollfd p{STDIN_FILENO, POLLIN, 0};
        if (::poll(&p, 1, 0) <= 0) return false;

        char chunk[512];
        const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
        if (n <= 0) { eof = true; return false; }
        buf.append(chunk, static_cast<size_t>(n));
    }
}

// The host's clock, standing in for the GPS's UTC time and RMC date, moved
// by `shift_h` hours so that night can be had in the afternoon.
void host_utc(int shift_h, sim::Controls& c) {
    using namespace std::chrono;
    const long long ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count() +
        static_cast<long long>(shift_h) * 3600000LL;
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm u{};
    gmtime_r(&secs, &u);
    c.utc_ms = static_cast<uint32_t>(ms % (24LL * 60 * 60 * 1000));
    c.year = static_cast<uint16_t>(1900 + u.tm_year);
    c.month = static_cast<uint8_t>(u.tm_mon + 1);
    c.day = static_cast<uint8_t>(u.tm_mday);
    c.day_of_year = static_cast<uint16_t>(u.tm_yday + 1);
}

// The devices the front end can break, by the name `dev` takes. The wheels
// are not here: `logic` reports them (core::fusion::WheelCheck), and the
// dead-sensor toggles are how to break one. Nor the phone, which FakeBle
// owns, nor the ECU and TPMS, which this board does not have.
enum class DevMode : uint8_t { Ok, Failing, Absent };
struct NamedDev {
    const char*       name;
    core::state::Dev  dev;
};
constexpr NamedDev kNamedDevs[] = {
    {"imu", core::state::Dev::Imu},          {"gnss", core::state::Dev::Gnss},
    {"lamps", core::state::Dev::LedDriver},  {"pod", core::state::Dev::McpPod},
    {"sd", core::state::Dev::Sd},            {"display", core::state::Dev::Display},
};

// `storage`'s part: CONFIG -> SD card -> Eject, answered as the card task
// would. The card stays out of the health table until it is pulled (`dev
// sd absent`) and put back, which is the only way out on the bike too.
struct FakeStorage {
    uint32_t gen = 0;
    bool     ejected = false;

    void begin(core::state::Slots& slots) { gen = slots.storage_request.generation(); }

    void step(core::state::Slots& slots, core::health::Tracker& health,
              ui::SystemInfo& system, bool card_in) {
        const uint32_t g = slots.storage_request.generation();
        if (g != gen) {
            gen = g;
            if (slots.storage_request.get().op == core::state::StorageRequest::Eject &&
                card_in && !ejected) {
                ejected = true;
                health.set_absent(core::state::Dev::Sd);
                system.sd_ejected = true;
            }
        }
        if (!card_in && ejected) {
            ejected = false;
            system.sd_ejected = false;
        }
    }
};

// `wifi`'s part, minus the radio: the hotspot starts, gets a new password,
// counts down its idle timer and goes off when the bike moves, by
// core::wifi's own rules. `in_use` is a phone on the page, which is what
// keeps it alive on the bike.
struct FakeWifi {
    core::state::Wifi w;
    uint32_t gen = 0;
    uint32_t started_ms = 0;
    uint32_t last_use_ms = 0;
    uint32_t rng = 0x2545F491u;

    uint32_t next() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    }

    void begin(core::state::Slots& slots, uint32_t seed) {
        rng ^= seed;
        gen = slots.wifi_request.generation();
        slots.wifi.publish(w);
    }

    void step(core::state::Slots& slots, uint32_t now, float speed_mph, bool in_use) {
        using core::wifi::State;
        const core::state::Wifi before = w;
        const uint32_t g = slots.wifi_request.generation();
        if (g != gen) {
            gen = g;
            const uint8_t op = slots.wifi_request.get().op;
            const bool off = w.state == State::Off || w.state == State::Failed;
            if (op == core::state::WifiRequest::Toggle && off) {
                w.state = State::Starting;
                started_ms = now;
                core::wifi::make_password(next(), next(), w.pass);
            } else if (op != core::state::WifiRequest::None) {
                w = core::state::Wifi{};
            }
        }
        // The C6 takes a moment to bring the access point up.
        if (w.state == State::Starting && now - started_ms >= 400) {
            w.state = State::On;
            last_use_ms = now;
        }
        if (w.state == State::On) {
            if (in_use) last_use_ms = now;
            w.clients = in_use ? 1 : 0;
            if (core::wifi::should_stop(now, last_use_ms, speed_mph)) {
                w = core::state::Wifi{};
            } else {
                w.off_in_s = static_cast<uint16_t>(core::wifi::off_in_s(now, last_use_ms));
            }
        }
        if (std::memcmp(&before, &w, sizeof w) != 0) slots.wifi.publish(w);
    }
};

// `ble`'s part: one bonded phone that connects when the front end says so,
// and CONFIG's Phone actions. Forget drops the bond, after which the phone
// only gets back in during a pairing window, as core/ble decides on the
// bike. Fills DIAG -> Radio and the Phone row of the health table.
struct FakeBle {
    core::state::BleDiag b;
    uint32_t gen = 0;
    uint32_t pair_until_ms = 0;
    bool     connected = false;

    void begin(core::state::Slots& slots) {
        b.bonds = 1;
        std::snprintf(b.c6_fw, sizeof b.c6_fw, "sim");
        gen = slots.ble_request.generation();
        slots.ble_diag.publish(b);
    }

    void step(core::state::Slots& slots, core::health::Tracker& health, uint32_t now,
              bool phone_on) {
        const core::state::BleDiag before = b;
        const uint32_t g = slots.ble_request.generation();
        if (g != gen) {
            gen = g;
            const uint8_t op = slots.ble_request.get().op;
            if (op == core::state::BleRequest::ForgetPhone) {
                b.bonds = 0;
                connected = false;
            } else if (op == core::state::BleRequest::PairingMode) {
                pair_until_ms = now + 60000;
            }
        }
        const bool window = static_cast<int32_t>(pair_until_ms - now) > 0;
        b.pairing_s = window ? static_cast<uint8_t>((pair_until_ms - now + 999) / 1000) : 0;
        if (!phone_on) {
            connected = false;
        } else if (!connected && (b.bonds > 0 || window)) {
            connected = true;
            if (b.bonds == 0) b.bonds = 1;   // bonded in the window
        }
        b.encrypted = connected;
        b.ctrl_subscribed = connected;
        if (connected) {
            b.has_rssi = true;
            // A phone in a jacket pocket a metre away, moving a little.
            b.rssi_dbm = static_cast<int16_t>(-58 - static_cast<int>((now / 1000) % 5));
            const core::state::Media m = slots.media.get();
            b.last_tag = m.updated_ms != 0 ? 'P' : 0;
        }
        if (std::memcmp(&before, &b, sizeof b) != 0) slots.ble_diag.publish(b);

        constexpr auto kPhone = core::state::Dev::Phone;
        if (connected) {
            if (health.is_absent(kPhone)) health.begin_init(kPhone, now);
            health.report_ok(kPhone, now);
        } else if (!health.is_absent(kPhone)) {
            health.set_absent(kPhone);
        }
    }
};

// The host's UTC offset in whole hours, which is all the dash's setting
// holds. Half-hour zones round toward zero.
int host_tz_offset_h() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    return static_cast<int>(local.tm_gmtoff / 3600);
}

// `ota`'s part, faked. The sim has no flash and no second slot, so this
// answers the Update screen's requests the way the dash would if each one
// worked, and instantly: enough to see every state the screen can show and
// drive it from the pod buttons.
struct FakeOta {
    core::state::Ota o;
    uint32_t gen = 0;

    void begin(core::state::Slots& slots) {
        o.known = true;
        o.role = core::ota::Role::Stable;
        std::snprintf(o.running_ver, sizeof o.running_ver, "sim");
        std::snprintf(o.running_slot, sizeof o.running_slot, "ota_0");
        o.other = core::ota::Other::Empty;
        gen = slots.ota_request.generation();
        slots.ota.publish(o);
    }

    void step(core::state::Slots& slots) {
        namespace ota = core::ota;
        bool dirty = false;
        const uint32_t g = slots.ota_request.generation();
        if (g != gen) {
            gen = g;
            dirty = true;
            const core::state::Motion m = slots.motion.get();
            const core::state::Engine e = slots.engine.get();
            const bool parked = ota::may_write(m.best_mph, e.rpm);
            auto fail = [&](ota::InstallError err) {
                o.install = ota::Install::Failed;
                o.error = err;
            };
            const uint8_t op = slots.ota_request.get().op;
            if (op == core::state::OtaRequest::Promote) {
                if (o.role != ota::Role::Dev) fail(ota::InstallError::NotDev);
                else if (!parked) fail(ota::InstallError::Moving);
                else {
                    o.role = ota::Role::Stable;
                    o.install = ota::Install::Idle;
                    o.error = ota::InstallError::None;
                }
            } else if (o.role != ota::Role::Stable) {
                fail(ota::InstallError::NotStable);
            } else if (!parked) {
                fail(ota::InstallError::Moving);
            } else if (op == core::state::OtaRequest::InstallSd) {
                o.other = ota::Other::Ready;
                std::snprintf(o.other_ver, sizeof o.other_ver, "sim-dev");
                o.install = ota::Install::Done;
                o.error = ota::InstallError::None;
            } else if (o.other != ota::Other::Ready) {
                fail(ota::InstallError::NoDev);
            } else {
                // A restart into the other slot: swap what each one holds.
                char tmp[sizeof o.running_ver];
                std::memcpy(tmp, o.running_ver, sizeof tmp);
                std::memcpy(o.running_ver, o.other_ver, sizeof tmp);
                std::memcpy(o.other_ver, tmp, sizeof tmp);
                std::snprintf(o.running_slot, sizeof o.running_slot,
                              o.running_slot[4] == '0' ? "ota_1" : "ota_0");
                o.install = ota::Install::Idle;
                o.error = ota::InstallError::None;
                o.role = ota::Role::Dev;
            }
        }
        if (dirty) slots.ota.publish(o);
    }
};

void apply_set(sim::Controls& c, const std::string& name, float v) {
    if (name == "speed")        c.speed_mph = v;
    else if (name == "rpm")     c.rpm = v;
    else if (name == "lean")    c.lean_deg = v;
    else if (name == "pitch")   c.pitch_deg = v;
    else if (name == "tps")     c.tps_pct = v;
    else if (name == "coolant") c.coolant_c = v;
    else if (name == "vbat")    c.vbat = v;
    else if (name == "fslip")   c.front_slip_pct = v;
    else if (name == "rslip")   c.rear_slip_pct = v;
    else if (name == "deadf")   c.front_dead = v != 0.0f;
    else if (name == "deadr")   c.rear_dead = v != 0.0f;
    else if (name == "down")    c.down = v != 0.0f;
    else if (name == "temp")    c.dash_temp_c = v;
    else if (name == "sats")    c.sats = static_cast<int>(v);
    else if (name == "neutral") c.neutral = v != 0.0f;
    else if (name == "ecu")     c.ecu_online = v != 0.0f;
    else if (name == "fix")     c.gps_fix = v != 0.0f;
    else if (name == "auto")    c.auto_ride = v != 0.0f;
    else if (name == "duration") c.media_duration_ms = static_cast<uint32_t>(v);
    else if (name == "playstate") c.media_state = static_cast<uint8_t>(v);
}

// One word for the FRAME line: spaces as underscores, `-` for nothing.
template <size_t N>
const char* token(char (&out)[N], const char* s) {
    if (!s || !*s) s = "-";
    size_t i = 0;
    for (; s[i] != '\0' && i + 1 < N; ++i) out[i] = s[i] == ' ' ? '_' : s[i];
    out[i] = '\0';
    return out;
}

template <size_t N>
const char* copy_field(char (&dst)[N], const char* src) {
    size_t i = 0;
    while (src[i] != '\0' && src[i] != '\t' && i + 1 < N) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    // Skip whatever did not fit, then the separator, so a long title cannot
    // push the album into the artist's field.
    while (src[i] != '\0' && src[i] != '\t') ++i;
    return src[i] == '\t' ? src + i + 1 : src + i;
}

// `media <title>\t<album>\t<artist>\t<lyric>`. Tab separated rather than
// anything a band might reasonably use: "Song | Remix" is a real track name
// and "Song <tab> Remix" is not. The lyric field is optional — a line with
// only the first three fields leaves it blank, same as a real phone with no
// .lrc for the track. A bare `media` clears the whole block.
void apply_media(sim::Controls& c, const char* rest) {
    rest = copy_field(c.media_title, rest);
    rest = copy_field(c.media_album, rest);
    rest = copy_field(c.media_artist, rest);
    copy_field(c.media_lyric, rest);
}

}  // namespace

int main() {
    core::state::Slots     slots;
    core::config::Settings settings;
    // The default is the bike's home zone; the laptop running this may not
    // be in it, and a clock an hour off reads as a bug in the clock.
    settings.tz_offset_h = host_tz_offset_h();
    core::health::Tracker  health;
    ui::SystemInfo         system;
    DequeSource            commands;

    static core::state::TrackBuffer<core::state::TrackPoint, 4096> trip_trace;
    static core::state::TrackBuffer<core::state::TrackPoint, 4096> lap_trace;
    static core::log::Ring<core::log::Record, 512> blackbox;
    static core::delta::Tracker delta;
    static services::LogicState logic;
    static services::LapHistory lap_history;

    services::Deps deps;
    deps.slots = &slots;
    deps.settings = &settings;
    deps.health = &health;
    deps.system = &system;
    deps.trip_trace = &trip_trace;
    deps.lap_trace = &lap_trace;
    deps.blackbox = &blackbox;
    deps.delta = &delta;
    deps.lap_history = &lap_history;

    // Everything reports healthy except the things that genuinely are not
    // on this board, so the screens show what they will show on the bike.
    const uint32_t boot = 0;
    for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
        const auto dev = static_cast<core::state::Dev>(i);
        // Not the wheels: on the bike they stay absent until `logic`'s wheel
        // check has judged them, which needs the bike moving. Brought in
        // here, nothing reported them while parked and the tracker faulted
        // both as stale two seconds later.
        if (dev == core::state::Dev::FrontWheel || dev == core::state::Dev::RearWheel) continue;
        health.begin_init(dev, boot);
        health.report_ok(dev, boot);
    }
    health.set_absent(core::state::Dev::Ecu);
    health.set_absent(core::state::Dev::Phone);   // until FakeBle connects one
    health.set_absent(core::state::Dev::TpmsF);
    health.set_absent(core::state::Dev::TpmsR);

    ui::Context ctx;
    ctx.settings = &settings;
    ctx.commands = &commands;
    ctx.system = &system;

    ui::App app(ctx);
    app.bind_settings(&settings);

    // The map widgets read the traces through the same view the render task
    // gives them on the device.
    struct TraceSource final : public ui::IPointSource {
        using Buffer = core::state::TrackBuffer<core::state::TrackPoint, 4096>;
        explicit TraceSource(Buffer* b) : buf(b) {}
        size_t count() const override { return buf ? buf->size() : 0; }
        size_t total() const override { return buf ? buf->total() : 0; }
        bool point(size_t i, float& x, float& y) const override {
            core::state::TrackPoint p;
            if (!buf || !buf->read_nth(i, p)) return false;
            x = p.x_ft;
            y = p.y_ft;
            return true;
        }
        Buffer* buf;
    };
    static TraceSource trip_src(&trip_trace);
    static TraceSource lap_src(&lap_trace);
    app.set_trip_source(&trip_src);
    app.set_track_source(&lap_src);

    std::vector<ui::Color> px(static_cast<size_t>(ui::kScreenW) * ui::kScreenH, 0);
    ui::FrameBufferCanvas canvas(px.data(), ui::kScreenW, ui::kScreenH);
    app.begin(canvas);

    ui::wire::Panel wire_panel = ui::wire::i80_8bit(10000000u);   // bsp::kTftWrHz
    wire_panel.bus_limit_bytes = kBusLimitBytes;
    ui::wire::Model wire(wire_panel, kBandPixels, kBandLines);
    // render_service's copy of the glass, and its merge rule.
    std::vector<ui::Color> glass(static_cast<size_t>(ui::kScreenW) * ui::kScreenH, 0);
    ui::Shadow shadow(glass.data());
    std::vector<ui::Color> strip_px;
    const uint32_t overhead_px = ui::wire::overhead_px(wire_panel);
    uint32_t last_scrub = 0;
    int scrub_band = 0;
    // What the glass shows: the canvas as the bus has delivered it so far.
    sim::PanelSim panel(kBandPixels, kBandLines);
    panel.set_bus_limit(kBusLimitBytes);

    sim::Bike     bike;
    sim::Controls controls;
    core::input::Decoder  buttons;
    core::input::Config   button_cfg;
    uint8_t button_mask = 0;
    uint8_t lamp_mask = 0;
    // The tach's rate of climb, for predictive lamps: fed the bike's rpm
    // every tick, where ctrl feeds it every tach edge.
    core::tach::RateEstimator rpm_rate;

    services::logic_restore(deps, logic);

    // What `logger` does with the blackbox ring, minus the SD card. The
    // point is that you can ride the simulated circuit and get a real
    // RaceRender file out of it, so the overlay pipeline can be checked
    // before there is a board to log from.
    std::FILE* log_file = std::fopen("dash_log.csv", "wb");
    if (log_file) {
        char head[256];
        const size_t h = core::log::csv_header(head, sizeof head);
        std::fwrite(head, 1, h, log_file);
    }

    uint32_t now = 0;
    uint32_t last_frame = 0;
    FakeOta fake_ota;
    fake_ota.begin(slots);
    FakeStorage fake_storage;
    fake_storage.begin(slots);
    FakeWifi fake_wifi;
    fake_wifi.begin(slots, static_cast<uint32_t>(std::time(nullptr)));
    FakeBle fake_ble;
    fake_ble.begin(slots);

    DevMode dev_mode[core::state::kDevCount] = {};   // all Ok
    int  clock_shift_h = 0;
    bool phone_on = false;
    bool wifi_in_use = false;

    // `keeper`'s backlight: the CONFIG brightness, dimmed after dark by the
    // same core::sun::AutoDim, once a second. The sim has no backlight, so
    // the front end darkens the picture by it instead.
    core::sun::AutoDim auto_dim;
    uint8_t  backlight_pct = settings.brightness_pct;
    uint32_t last_backlight_ms = 0;
    std::vector<unsigned char> rgb(static_cast<size_t>(ui::kScreenW) *
                                   ui::kScreenH * 3);

    auto wall = std::chrono::steady_clock::now();

    for (;;) {
        // Run at wall-clock speed. The point of the thing is to judge how
        // the dash reads at a glance, and that is not a question you can
        // answer from frames rendered as fast as a laptop can manage.
        wall += std::chrono::milliseconds(kTickMs);
        std::this_thread::sleep_until(wall);

        // --- input from the front end -----------------------------------
        std::string line;
        while (poll_line(line)) {
            char a[32] = {}, b[32] = {};
            float v = 0.0f;
            // Before the sscanf forms: this one takes the rest of the line
            // verbatim, spaces and all, because song titles have spaces in.
            if (line.rfind("media", 0) == 0) {
                const char* rest = line.c_str() + 5;
                if (*rest == ' ') ++rest;
                apply_media(controls, rest);
            } else if (std::sscanf(line.c_str(), "set %31s %f", a, &v) == 2) {
                // Position is an anchor, so it needs the sim clock and
                // cannot go through apply_set with the plain controls.
                if (std::strcmp(a, "bus_mhz") == 0) {
                    panel.set_clock_hz(v > 0.0f ? static_cast<uint32_t>(v * 1e6f) : 0u);
                } else if (std::strcmp(a, "shift") == 0) {
                    clock_shift_h = static_cast<int>(v);
                } else if (std::strcmp(a, "phone") == 0) {
                    phone_on = v != 0.0f;
                } else if (std::strcmp(a, "wclient") == 0) {
                    wifi_in_use = v != 0.0f;
                } else if (std::strcmp(a, "position") == 0) {
                    controls.media_position_known = true;
                    controls.media_position_ms = v > 0.0f ? static_cast<uint32_t>(v) : 0;
                    controls.media_position_at_ms = now;
                } else {
                    apply_set(controls, a, v);
                }
            } else if (std::sscanf(line.c_str(), "btn %31s %31s", a, b) == 2) {
                const int idx = a[0] - 'A';
                if (idx >= 0 && idx < core::input::kButtonCount) {
                    const uint8_t bit = static_cast<uint8_t>(1u << idx);
                    if (std::strcmp(b, "down") == 0) button_mask |= bit;
                    else button_mask = static_cast<uint8_t>(button_mask & ~bit);
                }
            } else if (std::sscanf(line.c_str(), "dev %31s %31s", a, b) == 2) {
                for (const NamedDev& nd : kNamedDevs) {
                    if (std::strcmp(a, nd.name) != 0) continue;
                    DevMode& m = dev_mode[static_cast<uint8_t>(nd.dev)];
                    if (std::strcmp(b, "ok") == 0) m = DevMode::Ok;
                    else if (std::strcmp(b, "fail") == 0) m = DevMode::Failing;
                    else if (std::strcmp(b, "absent") == 0) m = DevMode::Absent;
                }
            } else if (std::sscanf(line.c_str(), "cmd %31s", a) == 1) {
                using core::state::Command;
                using core::state::CommandId;
                if (std::strcmp(a, "lapline") == 0) commands.post(Command(CommandId::LapSetLine));
                else if (std::strcmp(a, "laparm") == 0) commands.post(Command(CommandId::LapArm));
                else if (std::strcmp(a, "lapclear") == 0) commands.post(Command(CommandId::LapClear));
                else if (std::strcmp(a, "tripreset") == 0) commands.post(Command(CommandId::TripReset, int32_t{0}));
                else if (std::strcmp(a, "jump") == 0) bike.trigger_jump(now);
                else if (std::strcmp(a, "spin") == 0) bike.trigger_slip(sim::Bike::Slip::Spin, now);
                else if (std::strcmp(a, "flock") == 0) bike.trigger_slip(sim::Bike::Slip::FrontLock, now);
                else if (std::strcmp(a, "rlock") == 0) bike.trigger_slip(sim::Bike::Slip::RearLock, now);
                else if (std::strcmp(a, "crank") == 0) bike.trigger_crank(now, controls.vbat);
            } else if (line.rfind("quit", 0) == 0) {
                if (log_file) std::fclose(log_file);
                return 0;
            }
        }

        // --- one tick of the bike and the firmware ----------------------
        now += kTickMs;
        host_utc(clock_shift_h, controls);
        bike.step(slots, controls, now, kTickMs);
        fake_ota.step(slots);
        fake_storage.step(slots, health, system,
                          dev_mode[static_cast<uint8_t>(core::state::Dev::Sd)] != DevMode::Absent);
        fake_wifi.step(slots, now, bike.speed_mph(), wifi_in_use);
        fake_ble.step(slots, health, now, phone_on);

        // What `input` does: poll the pod, debounce, hand events to the UI.
        core::nav::InputEvent ev[core::input::kButtonCount];
        const size_t n = buttons.update(button_mask, now, button_cfg, ev,
                                        core::input::kButtonCount);
        for (size_t i = 0; i < n; ++i) app.on_input(ev[i], canvas);

        // What `logic` does — the same function the firmware task calls.
        services::logic_tick(deps, logic, commands, now);

        // What `ctrl` does with the lamps, via the same call.
        const core::config::ShiftView sv = core::config::shift_view(settings);
        core::shift::Config sc;
        sc.base_rpm = sv.base_rpm;
        sc.span_rpm = sv.span_rpm;
        sc.hysteresis_rpm = sv.hysteresis_rpm;
        sc.flash_rpm = sv.flash_rpm;
        sc.lead_ms = sv.lead_ms;
        if (bike.rpm() > 0.0f) {
            rpm_rate.add(static_cast<uint64_t>(now) * 1000u, bike.rpm(),
                         core::tach::Config{}.rate_window_ms * 1000u);
        } else {
            rpm_rate.reset();
        }
        uint16_t rpm = core::shift::lamp_rpm(bike.rpm(), rpm_rate.rpm_per_s(), sc);
        // ...including the start-up sweep, timed from the first frame.
        if (system.first_frame_ms != 0) {
            core::sweep::Config wc;
            wc.enabled = settings.gauge_sweep;
            wc.duration_ms = settings.sweep_ms;
            wc.peak_rpm = settings.shift_flash_rpm;
            const float sw = core::sweep::lamp_rpm(now - system.first_frame_ms,
                                                   bike.speed_mph(), wc);
            if (sw > rpm) rpm = static_cast<uint16_t>(sw);
        }
        lamp_mask = core::shift::pattern(rpm, sc, lamp_mask);
        const bool flash = core::shift::over_limit(rpm, sc);
        controls.shift_mask = lamp_mask;
        controls.shift_flash = flash;
        // What `ctrl` does: infer the gear from rpm against road speed.
        controls.gear = core::gear::estimate(bike.rpm(), bike.speed_mph(),
                                             controls.neutral,
                                             core::config::gear_view(settings));

        if (log_file) {
            core::log::Record rec;
            while (blackbox.pop(rec)) {
                char line[256];
                if (rec.lap_done_ms != 0) {
                    char tag[64];
                    const size_t t = core::log::format_lap_marker(
                        tag, sizeof tag, rec.lap_num, rec.lap_done_ms);
                    std::fwrite(tag, 1, t, log_file);
                }
                const size_t len = core::log::format_csv(line, sizeof line, rec);
                std::fwrite(line, 1, len, log_file);
            }
        }

        for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
            const auto dev = static_cast<core::state::Dev>(i);
            // The wheels are `logic`'s to report (core::fusion::WheelCheck),
            // as on the bike: an OK from here every tick drowned its fault,
            // and a dead sensor never reached DIAG or the alert. The phone
            // is FakeBle's.
            if (dev == core::state::Dev::FrontWheel ||
                dev == core::state::Dev::RearWheel ||
                dev == core::state::Dev::Phone) continue;
            // An ejected card stays out until it is pulled and put back.
            if (dev == core::state::Dev::Sd && fake_storage.ejected) continue;
            switch (dev_mode[i]) {
                case DevMode::Absent:
                    if (!health.is_absent(dev)) health.set_absent(dev);
                    break;
                case DevMode::Failing:
                case DevMode::Ok:
                    // Coming back (or arriving): the driver's init first, as
                    // on the bike, or its reports are ignored. Not the ECU or
                    // TPMS, which this board has not got.
                    if (health.is_absent(dev)) {
                        if (dev == core::state::Dev::Ecu || dev == core::state::Dev::TpmsF ||
                            dev == core::state::Dev::TpmsR) break;
                        health.begin_init(dev, now);
                    }
                    if (dev_mode[i] == DevMode::Ok) health.report_ok(dev, now);
                    else health.report_error(dev, -1, now);
                    break;
            }
        }
        health.tick(now, core::health::Policy{});
        health.publish_if_changed(slots.health, now);
        system.uptime_s = now / 1000;
        system.chip_temp_c = controls.dash_temp_c;
        system.chip_temp_ok = true;

        if (now - last_backlight_ms >= 1000) {
            last_backlight_ms = now;
            const core::state::Gnss g = slots.gnss.get();
            const uint8_t dim = auto_dim.update(g.fix && g.day_of_year != 0, g.day_of_year,
                                                g.utc_ms, g.lat, g.lon,
                                                settings.dim_at_night);
            backlight_pct = core::sun::apply_dim(settings.brightness_pct, dim);
        }

        // The bus runs whether or not a frame is due.
        panel.advance(static_cast<uint64_t>(kTickMs) * 1000000ull);

        // --- one frame --------------------------------------------------
        if (now - last_frame < kFrameMs) continue;
        last_frame = now;
        // render is blocked in its blit while the last frame is still on the
        // wire, so a slow bus drops frames instead of queueing them. The
        // glass keeps filling in; just redraw it.
        const bool bus_busy = panel.busy();

        ui::wire::Cost cost{};
        if (!bus_busy) {
            app.set_lap_history(lap_history.ms, lap_history.count,
                                lap_history.best_ms);
            app.update(core::state::snapshot(slots, now));

            // Take the damage before rendering it: `update` is what sets the
            // dirty flags and `render` is what clears them, so this is the
            // one moment the real set of moved widgets exists. The decision
            // mirrors render_service's -- full repaint or dirty widgets --
            // and so does what goes on the wire: each damaged strip compared
            // with the glass (ui::Shadow) and only its changed parts sent.
            const bool full = app.needs_full_repaint();
            ui::App::Damage dmg[ui::App::kMaxDamage];
            const size_t ndmg = full ? 0 : app.damage(dmg, ui::App::kMaxDamage);

            // The canvas is the firmware's idea of the screen and is always
            // whole. Only what was costed goes down the bus; the rest of the
            // panel keeps what it had, which is how a widget that was not
            // made a widget goes visibly stale.
            app.render(canvas, true);
            app.clear_full_repaint();
            // One strip, drawn: sent where it differs from the glass.
            auto send_strip = [&](const ui::Rect& r) {
                strip_px.resize(static_cast<size_t>(r.w) * r.h);
                for (int16_t y = 0; y < r.h; ++y) {
                    const ui::Color* src = px.data() +
                        static_cast<size_t>(r.y + y) * ui::kScreenW + r.x;
                    std::copy(src, src + r.w,
                              strip_px.begin() + static_cast<size_t>(y) * r.w);
                }
                ui::Rect parts[ui::Shadow::kMaxParts];
                const int np = shadow.diff(r, strip_px.data(), parts,
                                           ui::Shadow::kMaxParts, overhead_px);
                for (int j = 0; j < np; ++j) {
                    wire.transfer(parts[j]);
                    panel.queue_part(parts[j], px.data());
                }
            };
            if (full && !shadow.valid()) {
                // The glass is unknown: all of it, band by band.
                wire.full_repaint();
                panel.queue_full(px.data());
                shadow.record(ui::Rect{0, 0, ui::kScreenW, ui::kScreenH}, px.data());
                shadow.validate();
            } else if (full) {
                // A screen change: only what differs from the screen before.
                for (int16_t y = 0; y < ui::kScreenH; y = static_cast<int16_t>(y + kBandLines)) {
                    send_strip(ui::Rect{0, y, ui::kScreenW, kBandLines});
                }
            } else {
                for (size_t i = 0; i < ndmg; ++i) {
                    ui::Rect strips[ui::kMaxStrips];
                    const int ns = ui::plan_strips(dmg[i].r, kBandPixels, strips,
                                                   ui::kMaxStrips);
                    for (int k = 0; k < ns; ++k) send_strip(strips[k]);
                }
            }
            // render_service's scrub: one band of the glass resent a second.
            if (shadow.valid() && now - last_scrub >= 1000) {
                last_scrub = now;
                const ui::Rect r{0, static_cast<int16_t>(scrub_band * kBandLines),
                                 ui::kScreenW, kBandLines};
                scrub_band = (scrub_band + 1) % (ui::kScreenH / kBandLines);
                wire.transfer(r);
                panel.queue_part(r, glass.data());
            }
            cost = wire.end_frame();
        }
        if (system.first_frame_ms == 0) system.first_frame_ms = now ? now : 1;

        for (size_t i = 0; i < px.size(); ++i) {
            const ui::Color c = panel.shown()[i];
            rgb[i * 3 + 0] = static_cast<unsigned char>((c >> 8) & 0xF8);
            rgb[i * 3 + 1] = static_cast<unsigned char>((c >> 3) & 0xFC);
            rgb[i * 3 + 2] = static_cast<unsigned char>((c << 3) & 0xF8);
        }

        // The status line carries enough to debug the lap and delta path
        // without a screenshot: laps, the delta and whether it is believed,
        // and how far the matcher thinks the bike is from the reference.
        const core::state::Lap lapv = slots.lap.get();
        char slip_tok[16], alert_tok[32];
        token(slip_tok, app.slip_bar().active()
                            ? core::slip::kind_name(app.slip_bar().kind()) : "");
        token(alert_tok, app.alert_kind() != core::alert::Kind::None
                             ? core::alert::text(app.alert_kind()) : "");
        std::printf("FRAME %u %d %s %s %.4f laps=%u delta=%d valid=%d "
                    "ref=%zu match=%.1f wire=%.2f px=%u tx=%u peak=%.2f "
                    "over=%u busy=%d pend=%u slip=%s/%u alert=%s bl=%u\n",
                    lamp_mask, flash ? 1 : 0, app.mode_name(),
                    app.current().name(), bike.lap_fraction(),
                    lapv.count, lapv.delta_ms, lapv.delta_valid ? 1 : 0,
                    delta.reference_points(), delta.last_match_ft(),
                    static_cast<double>(cost.ms()), cost.pixels, cost.transfers,
                    static_cast<double>(wire.peak().ms()),
                    wire.total().oversized, bus_busy ? 1 : 0,
                    panel.pending_px(), slip_tok,
                    static_cast<unsigned>(app.slip_bar().pct()), alert_tok,
                    static_cast<unsigned>(backlight_pct));
        std::printf("P6\n%d %d\n255\n", ui::kScreenW, ui::kScreenH);
        std::fwrite(rgb.data(), 1, rgb.size(), stdout);
        std::fflush(stdout);
    }
}
