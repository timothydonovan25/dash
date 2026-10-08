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
//                                duration (media track length, ms),
//                                position (ms into the track, now),
//                                playstate (0 stop 1 play 2 pause)
//        btn <A-E> down|up       real debounce and long-press timing
//        cmd <lapline|laparm|lapclear|tripreset|jump>
//        media <title>\t<album>\t<artist>\t<lyric>    lyric is optional;
//                                                     bare `media` clears it
//        quit
//
//   out  FRAME <lamps> <flash> <mode> <screen> <lapfrac>\n
//        P6\n320 480\n255\n<pixels>
#include "sim_bike.hpp"

#include "core/config/config.hpp"
#include "core/health/health.hpp"
#include "core/input/input.hpp"
#include "core/gear/gear.hpp"
#include "core/shift/shift.hpp"
#include "core/sweep/sweep.hpp"
#include "services/logic_tick.hpp"
#include "ui/app.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/wire_cost.hpp"

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

constexpr uint32_t kTickMs  = 20;    // 50 Hz, the logic rate
constexpr uint32_t kFrameMs = 50;    // 20 Hz to the window

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

// The host's time of day, standing in for the GPS's UTC field.
uint32_t host_utc_ms_of_day() {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(ms % (24LL * 60 * 60 * 1000));
}

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
    else if (name == "sats")    c.sats = static_cast<int>(v);
    else if (name == "neutral") c.neutral = v != 0.0f;
    else if (name == "ecu")     c.ecu_online = v != 0.0f;
    else if (name == "fix")     c.gps_fix = v != 0.0f;
    else if (name == "auto")    c.auto_ride = v != 0.0f;
    else if (name == "duration") c.media_duration_ms = static_cast<uint32_t>(v);
    else if (name == "playstate") c.media_state = static_cast<uint8_t>(v);
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
        health.begin_init(static_cast<core::state::Dev>(i), boot);
        health.report_ok(static_cast<core::state::Dev>(i), boot);
    }
    health.set_absent(core::state::Dev::Ecu);
    health.set_absent(core::state::Dev::BleHost);
    health.set_absent(core::state::Dev::Phone);
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

    sim::Bike     bike;
    sim::Controls controls;
    core::input::Decoder  buttons;
    core::input::Config   button_cfg;
    uint8_t button_mask = 0;
    uint8_t lamp_mask = 0;

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
                if (std::strcmp(a, "position") == 0) {
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
            } else if (std::sscanf(line.c_str(), "cmd %31s", a) == 1) {
                using core::state::Command;
                using core::state::CommandId;
                if (std::strcmp(a, "lapline") == 0) commands.post(Command(CommandId::LapSetLine));
                else if (std::strcmp(a, "laparm") == 0) commands.post(Command(CommandId::LapArm));
                else if (std::strcmp(a, "lapclear") == 0) commands.post(Command(CommandId::LapClear));
                else if (std::strcmp(a, "tripreset") == 0) commands.post(Command(CommandId::TripReset, int32_t{0}));
                else if (std::strcmp(a, "jump") == 0) bike.trigger_jump(now);
            } else if (line.rfind("quit", 0) == 0) {
                if (log_file) std::fclose(log_file);
                return 0;
            }
        }

        // --- one tick of the bike and the firmware ----------------------
        now += kTickMs;
        controls.utc_ms = host_utc_ms_of_day();
        bike.step(slots, controls, now, kTickMs);
        fake_ota.step(slots);

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
        uint16_t rpm = static_cast<uint16_t>(controls.rpm);
        // ...including the start-up sweep, timed from the first frame.
        if (system.first_frame_ms != 0) {
            core::sweep::Config wc;
            wc.enabled = settings.gauge_sweep;
            wc.duration_ms = settings.sweep_ms;
            wc.peak_rpm = settings.shift_flash_rpm;
            const float sw = core::sweep::lamp_rpm(now - system.first_frame_ms,
                                                   controls.speed_mph, wc);
            if (sw > rpm) rpm = static_cast<uint16_t>(sw);
        }
        lamp_mask = core::shift::pattern(rpm, sc, lamp_mask);
        const bool flash = core::shift::over_limit(rpm, sc);
        controls.shift_mask = lamp_mask;
        controls.shift_flash = flash;
        // What `ctrl` does: infer the gear from rpm against road speed.
        controls.gear = core::gear::estimate(controls.rpm, controls.speed_mph,
                                             controls.neutral,
                                             core::config::gear_view(settings));

        if (log_file) {
            core::log::Record rec;
            while (blackbox.pop(rec)) {
                char line[192];
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

        health.tick(now, core::health::Policy{});
        for (uint8_t i = 0; i < core::state::kDevCount; ++i) {
            const auto dev = static_cast<core::state::Dev>(i);
            if (health.publish()[dev].state != core::state::DevState::Absent) {
                health.report_ok(dev, now);
            }
        }
        slots.health.publish(health.publish());
        system.uptime_s = now / 1000;

        // --- one frame --------------------------------------------------
        if (now - last_frame < kFrameMs) continue;
        last_frame = now;

        app.set_lap_history(lap_history.ms, lap_history.count,
                            lap_history.best_ms);
        app.update(core::state::snapshot(slots, now));

        // Price the frame before rendering it: `update` is what sets the
        // dirty flags and `render` is what clears them, so this is the one
        // moment the real set of moved widgets exists. The decision mirrors
        // render_service's — full repaint or dirty widgets, nothing else.
        if (app.needs_full_repaint()) {
            wire.full_repaint();
        } else {
            for (ui::Widget* w : app.current().widgets()) {
                if (w && w->dirty) wire.region(w->bounds);
            }
            if (app.alert().active()) wire.region(app.alert().bounds);
        }
        const ui::wire::Cost cost = wire.end_frame();

        // The window is one canvas, not eight bands, so it repaints whole.
        // What was just costed is what the panel would have sent.
        app.render(canvas, true);
        app.clear_full_repaint();
        if (system.first_frame_ms == 0) system.first_frame_ms = now ? now : 1;

        for (size_t i = 0; i < px.size(); ++i) {
            const ui::Color c = px[i];
            rgb[i * 3 + 0] = static_cast<unsigned char>((c >> 8) & 0xF8);
            rgb[i * 3 + 1] = static_cast<unsigned char>((c >> 3) & 0xFC);
            rgb[i * 3 + 2] = static_cast<unsigned char>((c << 3) & 0xF8);
        }

        // The status line carries enough to debug the lap and delta path
        // without a screenshot: laps, the delta and whether it is believed,
        // and how far the matcher thinks the bike is from the reference.
        const core::state::Lap lapv = slots.lap.get();
        std::printf("FRAME %u %d %s %s %.4f laps=%u delta=%d valid=%d "
                    "ref=%zu match=%.1f wire=%.2f px=%u tx=%u peak=%.2f "
                    "over=%u\n",
                    lamp_mask, flash ? 1 : 0, app.mode_name(),
                    app.current().name(), bike.lap_fraction(),
                    lapv.count, lapv.delta_ms, lapv.delta_valid ? 1 : 0,
                    delta.reference_points(), delta.last_match_ft(),
                    static_cast<double>(cost.ms()), cost.pixels, cost.transfers,
                    static_cast<double>(wire.peak().ms()),
                    wire.total().oversized);
        std::printf("P6\n%d %d\n255\n", ui::kScreenW, ui::kScreenH);
        std::fwrite(rgb.data(), 1, rgb.size(), stdout);
        std::fflush(stdout);
    }
}
