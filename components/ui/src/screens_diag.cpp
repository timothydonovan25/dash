#include "ui/screens.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"

namespace ui {

using core::state::Dev;
using core::state::Frame;
using core::state::kDevCount;
namespace u = core::units;

namespace {
// Bytes are the one number on the system screen where a raw count is worse
// than a rounded one: "184320" tells you nothing at a glance, "180 k" does.
void fmt_bytes(char* buf, size_t n, uint32_t bytes) {
    if (bytes >= 1024u * 1024u) {
        size_t k = u::fmt_float(buf, n, bytes / (1024.0f * 1024.0f), 1);
        if (k + 2 < n) { buf[k++] = ' '; buf[k++] = 'M'; buf[k] = '\0'; }
    } else {
        size_t k = u::fmt_float(buf, n, bytes / 1024.0f, 0);
        if (k + 2 < n) { buf[k++] = ' '; buf[k++] = 'k'; buf[k] = '\0'; }
    }
}
}  // namespace

// --- SensorsScreen ------------------------------------------------------
//
// The whole health table, one device per row. This is the bring-up tool when
// the PCB arrives: every driver reports into it, and a loose GH1.25 shows up
// here as a device flicking between Ok and Fault rather than as a screen
// that mysteriously stops updating.
namespace {
// Eleven devices will not fit down one side of a 320 px screen at a
// readable pitch, so they go in two columns. Six then five, reading down
// the left first, because that is where the eye starts.
constexpr int16_t kSensorTop   = 42;
constexpr int16_t kSensorStep  = 40;
constexpr int16_t kSensorRows  = 6;
constexpr int16_t kSensorColW  = 206;
}  // namespace

void SensorsScreen::enter(Canvas&) {
    for (uint8_t i = 0; i < kDevCount; ++i) {
        const int16_t col = i / kSensorRows;
        const int16_t row = i % kSensorRows;
        dots_[i].set_bounds(Rect{static_cast<int16_t>(14 + col * (kSensorColW + 26)),
                                 static_cast<int16_t>(kSensorTop + row * kSensorStep),
                                 kSensorColW, 24});
        dots_[i].configure(core::state::dev_name(static_cast<Dev>(i)));
        list_[i] = &dots_[i];
    }
}

void SensorsScreen::update(const Frame& f) {
    for (uint8_t i = 0; i < kDevCount; ++i) {
        dots_[i].set(f.health[static_cast<Dev>(i)].state);
    }
}

void SensorsScreen::draw_background(Canvas& c) {
    // This screen is the bring-up tool. It says so, because on the day the
    // board arrives it is the only screen anybody should be looking at.
    c.draw_text(static_cast<int16_t>(kScreenW - 14), chrome::kTitleY,
                "absent is not a fault", theme::secondary, 1, Align::Right);
}

WidgetList SensorsScreen::widgets() { return WidgetList{list_, kDevCount}; }

// --- GpsScreen ----------------------------------------------------------
namespace {
constexpr Rect kGpsTable {14, 40, 452, 264};
}  // namespace

void GpsScreen::enter(Canvas&) {
    table_.set_bounds(kGpsTable);
    table_.configure(10, 2);
    list_[0] = &table_;
}

void GpsScreen::update(const Frame& f) {
    char v[24];

    table_.set_row(0, "Fix", f.gnss.fix ? "yes" : "NO");

    u::fmt_value(v, sizeof v, static_cast<float>(f.gnss.sats), 0, true);
    table_.set_row(1, "Satellites", v);

    u::fmt_value(v, sizeof v, static_cast<float>(f.gnss.quality), 0, true);
    table_.set_row(2, "Quality", v);

    u::fmt_value(v, sizeof v, f.gnss.hdop, 1, f.gnss.fix);
    table_.set_row(3, "HDOP", v);

    // Six decimals is about 4 inches of latitude, which is the resolution
    // that matters when placing a finish line.
    u::fmt_value(v, sizeof v, static_cast<float>(f.gnss.lat), 5, f.gnss.fix);
    table_.set_row(4, "Latitude", v);
    u::fmt_value(v, sizeof v, static_cast<float>(f.gnss.lon), 5, f.gnss.fix);
    table_.set_row(5, "Longitude", v);

    u::fmt_value(v, sizeof v, f.gnss.alt_m, 0, f.gnss.fix);
    table_.set_row(6, "Altitude m", v);

    u::fmt_value(v, sizeof v, f.gnss.speed_mph, 1, f.gnss.fix);
    table_.set_row(7, "Speed mph", v);

    u::fmt_clock(v, sizeof v, f.gnss.utc_ms, 0, f.gnss.fix);
    table_.set_row(8, "UTC", v);

    // A marginal aerial or a chafed lead shows up as a rising bad-checksum
    // count long before it shows up as a lost fix.
    if (has_ctx() && ctx().system) {
        u::fmt_value(v, sizeof v, static_cast<float>(ctx().system->gnss_bad_csum),
                     0, true);
    } else {
        u::fmt_value(v, sizeof v, 0.0f, 0, false);
    }
    table_.set_row(9, "Bad checksums", v);

    table_.set_row_count(10);
}

WidgetList GpsScreen::widgets() { return WidgetList{list_, 1}; }

// --- SystemScreen -------------------------------------------------------
namespace {
constexpr Rect kSysTable {14, 40, 452, 264};
}  // namespace

void SystemScreen::enter(Canvas&) {
    table_.set_bounds(kSysTable);
    table_.configure(11, 2);
    list_[0] = &table_;
}

void SystemScreen::update(const Frame& f) {
    char v[24];
    const SystemInfo* s = has_ctx() ? ctx().system : nullptr;

    if (!s) {
        table_.clear_rows();
        table_.set_row(0, "no system info", "--");
        table_.set_row_count(1);
        return;
    }

    const uint32_t up = s->uptime_s;
    size_t k = u::fmt_float(v, sizeof v, static_cast<float>(up / 3600), 0);
    if (k + 6 < sizeof v) {
        v[k++] = 'h'; v[k++] = ' ';
        k += u::fmt_float(v + k, sizeof v - k, static_cast<float>((up / 60) % 60), 0);
        v[k++] = 'm'; v[k] = '\0';
    }
    table_.set_row(0, "Uptime", v);

    fmt_bytes(v, sizeof v, s->heap_free_b);
    table_.set_row(1, "Heap free", v);
    fmt_bytes(v, sizeof v, s->heap_min_b);
    table_.set_row(2, "Heap low water", v);
    fmt_bytes(v, sizeof v, s->psram_free_b);
    table_.set_row(3, "PSRAM free", v);

    u::fmt_float(v, sizeof v, s->render_hz_x10 / 10.0f, 1);
    table_.set_row(4, "Render Hz", v);

    // Anything but zero here means the shift lights were late, which is the
    // one latency figure on this bike that actually matters (§5.2).
    u::fmt_value(v, sizeof v, static_cast<float>(s->ctrl_late), 0, true);
    table_.set_row(5, "Ctrl overruns", v);

    u::fmt_value(v, sizeof v, static_cast<float>(s->log_records), 0, true);
    table_.set_row(6, "Log records", v);
    u::fmt_value(v, sizeof v, static_cast<float>(s->log_dropped), 0, true);
    table_.set_row(7, "Log dropped", v);

    u::fmt_value(v, sizeof v, f.elec.v3v3, 2, f.elec.v3v3 > 0.0f);
    table_.set_row(8, "3V3 rail", v);
    u::fmt_value(v, sizeof v, f.elec.v5, 2, f.elec.v5 > 0.0f);
    table_.set_row(9, "5V rail", v);

    u::fmt_value(v, sizeof v, static_cast<float>(s->imu_timeouts), 0, true);
    table_.set_row(10, "IMU int miss", v);

    table_.set_row_count(11);
}

WidgetList SystemScreen::widgets() { return WidgetList{list_, 1}; }

// --- BusScreen ----------------------------------------------------------
namespace {
constexpr Rect kBusTable {14, 40, 452, 264};
}  // namespace

void BusScreen::enter(Canvas&) {
    table_.set_bounds(kBusTable);
    table_.configure(12, 2);
    list_[0] = &table_;
}

void BusScreen::update(const Frame& f) {
    char v[24];

    // Which bus each device is on, and whether it answered. When the board
    // arrives this is the screen that says "the pod is dead" versus "I2C0 is
    // dead", and those are very different afternoons.
    const auto state_text = [](core::state::DevState s) {
        switch (s) {
            case core::state::DevState::Absent:   return "absent";
            case core::state::DevState::Init:     return "init";
            case core::state::DevState::Ok:       return "ok";
            case core::state::DevState::Degraded: return "degraded";
            case core::state::DevState::Fault:    return "FAULT";
        }
        return "?";
    };

    table_.set_row(0, "SPI2 imu", state_text(f.health[Dev::Imu].state));
    table_.set_row(1, "SPI2 mcp", state_text(f.health[Dev::McpBoard].state));
    table_.set_row(2, "SPI2 sd",  state_text(f.health[Dev::Sd].state));
    table_.set_row(3, "SPI3 tft", state_text(f.health[Dev::Display].state));
    table_.set_row(4, "I2C0 pod", state_text(f.health[Dev::McpPod].state));
    table_.set_row(5, "UART1 gps",state_text(f.health[Dev::Gnss].state));

    u::fmt_value(v, sizeof v, static_cast<float>(f.health[Dev::McpPod].fault_count),
                 0, true);
    table_.set_row(6, "Pod NACKs", v);

    u::fmt_value(v, sizeof v, static_cast<float>(f.health[Dev::Imu].fault_count),
                 0, true);
    table_.set_row(7, "IMU errors", v);

    table_.set_row(8, "Neutral in", f.engine.neutral ? "LOW (N)" : "high");

    u::fmt_value(v, sizeof v, f.elec.vbat, 1, f.elec.vbat > 0.0f);
    table_.set_row(9, "VIN sense", v);      // no divider this revision: reads --

    // The pod is the only device on a cable that leaves the box, so it is
    // the one that gets its bus wedged. `stuck` non-zero means clocking the
    // bus did not free SDA, which is a wiring fault rather than a glitch.
    u::fmt_value(v, sizeof v,
                 static_cast<float>(has_ctx() && ctx().system
                                        ? ctx().system->pod_recoveries : 0),
                 0, true);
    table_.set_row(10, "I2C recover", v);

    u::fmt_value(v, sizeof v,
                 static_cast<float>(has_ctx() && ctx().system
                                        ? ctx().system->pod_stuck : 0),
                 0, true);
    table_.set_row(11, "  still stuck", v);

    table_.set_row_count(12);
}

WidgetList BusScreen::widgets() { return WidgetList{list_, 1}; }

// --- RadioScreen --------------------------------------------------------
namespace {
constexpr Rect kRadioNote  {14,  40, 452,  16};
constexpr Rect kRadioTable {14,  72, 452, 232};
}  // namespace

void RadioScreen::enter(Canvas&) {
    note_.set_bounds(kRadioNote);
    note_.configure("", color::kDim, 1, Align::Left);
    table_.set_bounds(kRadioTable);
    table_.configure(6, 2);
    list_[0] = &note_;
    list_[1] = &table_;
}

void RadioScreen::update(const Frame& f) {
    // On the real S3 firmware `Dev::BleHost` never leaves Absent — nothing
    // there calls ble_media_start() (step 9 of the build order; see
    // CLAUDE.md) — so this note still reads exactly as it always has there.
    // It only changes where BLE is actually running (demo_p4).
    note_.set_text(f.health.ok(Dev::BleHost) ? "BLE up"
                                             : "BLE not built in this firmware");
    table_.set_row(0, "Host", f.health.ok(Dev::BleHost) ? "up" : "--");
    table_.set_row(1, "Phone", f.health.ok(Dev::Phone) ? "bonded" : "--");
    table_.set_row(2, "TPMS front", f.tpms.front.valid ? "seen" : "--");
    table_.set_row(3, "TPMS rear", f.tpms.rear.valid ? "seen" : "--");
    table_.set_row(4, "Media", f.media.updated_ms ? f.media.title : "--");
    table_.set_row(5, "Controller core", "1");
    table_.set_row_count(6);
}

WidgetList RadioScreen::widgets() { return WidgetList{list_, 2}; }

}  // namespace ui
