#include "ui/screens.hpp"
#include "core/fusion/speed.hpp"
#include "core/units/units.hpp"
#include "ui/chrome.hpp"

#include <cstdio>

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
// Thirteen devices will not fit down one side of a 320 px screen at a
// readable pitch, so they go in two columns. Seven then six, reading down
// the left first, because that is where the eye starts.
constexpr int16_t kSensorTop   = 42;
constexpr int16_t kSensorStep  = 38;
constexpr int16_t kSensorRows  = 7;
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
    table_.configure(12, 2);
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

    // Fused speed beside the raw figures, and which sources it is using:
    // F front wheel, R rear, G GPS; lower case is suspect.
    if (f.fused.speed_valid) {
        char sig[12];
        const size_t k = u::fmt_value(v, sizeof v, f.fused.speed_mph, 1, true);
        u::fmt_value(sig, sizeof sig, f.fused.speed_sigma_mph, 1, true);
        std::snprintf(v + k, sizeof v - k, " +-%s", sig);
    } else {
        std::snprintf(v, sizeof v, "--");
    }
    table_.set_row(10, "Fused mph", v);
    namespace fu = core::fusion;
    const auto src = [&](uint8_t bit, char up) {
        return (f.fused.suspect & bit) ? static_cast<char>(up + 32)
             : (f.fused.used & bit)    ? up : '.';
    };
    std::snprintf(v, sizeof v, "%c%c%c resets %u", src(fu::kSrcFront, 'F'),
                  src(fu::kSrcRear, 'R'), src(fu::kSrcGps, 'G'),
                  static_cast<unsigned>(f.fused.resets));
    table_.set_row(11, "Fusion", v);

    table_.set_row_count(12);
}

WidgetList GpsScreen::widgets() { return WidgetList{list_, 1}; }

// --- SystemScreen -------------------------------------------------------
namespace {
constexpr Rect kSysTable {14, 40, 452, 264};
}  // namespace

void SystemScreen::enter(Canvas&) {
    table_.set_bounds(kSysTable);
    table_.configure(12, 2);
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

    // "ejected" is the one state the health table cannot say: CONFIG's
    // Eject has unmounted a card that is still in the socket, so it is safe
    // to pull. Health calls that absent, which would read as no card.
    const char* sd = "--";
    switch (f.health[Dev::Sd].state) {
        case core::state::DevState::Absent:   sd = "absent";   break;
        case core::state::DevState::Init:     sd = "init";     break;
        case core::state::DevState::Ok:       sd = "ok";       break;
        case core::state::DevState::Degraded: sd = "degraded"; break;
        case core::state::DevState::Fault:    sd = "FAULT";    break;
    }
    if (s->sd_ejected) sd = "ejected, safe to pull";
    table_.set_row(11, "SD card", sd);

    table_.set_row_count(12);
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
    table_.set_row(1, "I2C1 leds",state_text(f.health[Dev::LedDriver].state));
    table_.set_row(2, "SPI2 sd",  state_text(f.health[Dev::Sd].state));
    table_.set_row(3, "i80 tft",  state_text(f.health[Dev::Display].state));
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
    table_.set_row(9, "VIN sense", v);      // -- until the ADC has a reading

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
    table_.configure(11, 2);
    list_[0] = &note_;
    list_[1] = &table_;
}

void RadioScreen::update(const Frame& f) {
    // `ble` reports the host Ok once NimBLE has synced with the C6's
    // controller, and Fault if the SDIO link or the controller never came
    // up. The details are in the console log; the screen only says which.
    const bool host_fault =
        f.health[Dev::BleHost].state == core::state::DevState::Fault;
    note_.set_text(f.health.ok(Dev::BleHost) ? "BLE up"
                   : host_fault              ? "BLE failed to start"
                                             : "BLE not running");
    table_.set_row(0, "Host", f.health.ok(Dev::BleHost) ? "up" : "--");
    // Connected is not the same as let in: a phone that has not encrypted
    // yet is on its way to being dropped (core::ble::kEncryptTimeoutMs).
    table_.set_row(1, "Phone", !f.health.ok(Dev::Phone) ? "--"
                               : f.ble_diag.encrypted  ? "bonded"
                                                       : "connected");

    char bonds[4] = {static_cast<char>('0' + (f.ble_diag.bonds % 10)), '\0'};
    table_.set_row(2, "Bonds", f.health.ok(Dev::BleHost) ? bonds : "--");

    char pairing[12] = "closed";
    if (f.ble_diag.pairing_s > 0) {
        size_t k = core::units::fmt_value(pairing, sizeof pairing,
                                          static_cast<float>(f.ble_diag.pairing_s),
                                          0, false);
        for (const char* p = " s left"; *p && k + 1 < sizeof pairing; ++p) {
            pairing[k++] = *p;
        }
        pairing[k] = '\0';
    }
    table_.set_row(3, "Pairing", pairing);

    char rssi[12] = "--";
    if (f.ble_diag.has_rssi) {
        size_t k = core::units::fmt_value(rssi, sizeof rssi,
                                          static_cast<float>(f.ble_diag.rssi_dbm), 0,
                                          true);
        if (k + 5 < sizeof rssi) {
            rssi[k++] = ' '; rssi[k++] = 'd'; rssi[k++] = 'B'; rssi[k++] = 'm';
            rssi[k] = '\0';
        }
    }
    table_.set_row(4, "RSSI", rssi);

    // The one byte, not the line: a title here would just repeat the Media
    // row below. '\0' before anything has ever arrived.
    char last[2] = {f.ble_diag.last_tag, '\0'};
    table_.set_row(5, "Last tag", f.ble_diag.last_tag ? last : "--");
    table_.set_row(6, "Ctrl", f.ble_diag.ctrl_subscribed ? "subscribed" : "--");
    table_.set_row(7, "TPMS front", f.tpms.front.valid ? "seen" : "--");
    table_.set_row(8, "TPMS rear", f.tpms.rear.valid ? "seen" : "--");
    table_.set_row(9, "Media", f.media.updated_ms ? f.media.title : "--");
    table_.set_row_count(10);
}

WidgetList RadioScreen::widgets() { return WidgetList{list_, 2}; }

// --- UpdateScreen -------------------------------------------------------
namespace {
constexpr int16_t kUpdValueX = 96;
constexpr Rect kUpdRunning {kUpdValueX,  40, 370, 10};
constexpr Rect kUpdOther   {kUpdValueX,  58, 370, 10};
constexpr Rect kUpdC6      {kUpdValueX,  76, 370, 10};
constexpr Rect kUpdStatus  {14,          100, 452, 10};
constexpr Rect kUpdActions {14,          124, 452, 88};
constexpr Rect kUpdWifiJoin{14,          220, 452, 16};
constexpr Rect kUpdWifiInfo{14,          242, 452, 10};
constexpr int16_t kUpdNoteY = 268;

// "<a> <b> <c>", for the version lines.
void join(char* out, size_t n, const char* a, const char* b, const char* c) {
    const char* parts[] = {a, b, c};
    size_t k = 0;
    for (const char* part : parts) {
        if (!part || !*part) continue;
        if (k > 0 && k + 1 < n) out[k++] = ' ';
        for (const char* p = part; *p && k + 1 < n; ++p) out[k++] = *p;
    }
    out[k] = '\0';
}
}  // namespace

void UpdateScreen::enter(Canvas&) {
    running_.set_bounds(kUpdRunning);
    other_.set_bounds(kUpdOther);
    c6_.set_bounds(kUpdC6);
    status_.set_bounds(kUpdStatus);
    running_.configure("--", color::kText);
    other_.configure("--", color::kText);
    c6_.configure("--", color::kText);
    status_.configure("", color::kDim);
    actions_.set_bounds(kUpdActions);
    actions_.configure(kActions, 2);
    list_[0] = &running_;
    list_[1] = &other_;
    list_[2] = &c6_;
    list_[3] = &status_;
    list_[4] = &actions_;
    wifi_join_.set_bounds(kUpdWifiJoin);
    wifi_join_.configure("", color::kText, 2, Align::Left);
    wifi_info_.set_bounds(kUpdWifiInfo);
    wifi_info_.configure("", color::kDim, 1, Align::Left);
    list_[5] = &wifi_join_;
    list_[6] = &wifi_info_;
}

void UpdateScreen::update(const Frame& f) {
    namespace ota = core::ota;
    const core::state::Ota& o = f.ota;

    // "stable ota_0 1.0 demo Wi-Fi OTA 2026-10-08": the version, board and
    // feature are the image's own (firmware.txt), the date its build date.
    char line[72], head[16];
    if (!o.known) {
        running_.set_text("--");
    } else {
        join(head, sizeof head, ota::role_name(o.role), o.running_slot, nullptr);
        join(line, sizeof line, head, o.running_ver, o.running_date);
        running_.set_text(line);
    }
    if (o.known && o.other == ota::Other::Ready) {
        join(line, sizeof line, o.other_ver, o.other_date, nullptr);
        other_.set_text(line);
    } else {
        other_.set_text(!o.known                         ? "--"
                        : o.other == ota::Other::Invalid ? "invalid image"
                                                         : "empty");
    }
    c6_.set_text(f.ble_diag.c6_fw[0] ? f.ble_diag.c6_fw : "--");

    // One line for whatever matters most right now.
    Color sc = color::kDim;
    const char* st = "";
    char err[48];
    if (o.install == ota::Install::Failed) {
        join(err, sizeof err, "failed:", ota::install_error_name(o.error),
             o.error == ota::InstallError::BadImage ? ota::image_error_name(o.image_error)
                                                    : nullptr);
        st = err;
        sc = color::kAlarm;
    } else if (o.install == ota::Install::Done) {
        st = "installed: boot dev once to try it";
        sc = color::kAccent;
    } else if (o.role == ota::Role::Dev) {
        st = "dev: Promote keeps it, any reset goes back";
        sc = color::kAlarm;
    }
    status_.configure(st, sc);

    // The right-hand column says why a row will not do anything, before
    // the rider presses it.
    const bool stable = o.known && o.role == ota::Role::Stable;
    const bool has_dev = o.other == ota::Other::Ready;
    char pct[8] = "";
    const char* install = "";
    if (!stable) {
        install = "stable only";
    } else if (o.install == ota::Install::Writing) {
        size_t k = u::fmt_value(pct, sizeof pct, static_cast<float>(o.install_pct), 0, true);
        if (k + 1 < sizeof pct) { pct[k++] = '%'; pct[k] = '\0'; }
        install = pct;
    } else if (o.install == ota::Install::Checking) {
        install = "checking";
    }
    const char* boot = !stable ? "stable only" : has_dev ? "" : "no dev";
    // Promoting is done from the image being kept, so it is a dev-only row.
    const char* promote = o.known && o.role == ota::Role::Dev ? "" : "dev only";
    actions_.set_row(0, "Install from SD", install);
    actions_.set_row(1, "Boot dev once", boot);
    actions_.set_row(2, "Promote to stable", promote);

    namespace wifi = core::wifi;
    const core::state::Wifi& w = f.wifi;
    actions_.set_row(3, "Wi-Fi", wifi::state_name(w.state));
    actions_.set_row_count(kActions);
    actions_.set_focus(focus_);

    char join_line[48] = "";
    char info_line[64] = "";
    if (w.state == wifi::State::On) {
        join(join_line, sizeof join_line, wifi::kSsid, " pass", w.pass);
        char n[8], mins[8];
        u::fmt_value(n, sizeof n, static_cast<float>(w.clients), 0, true);
        u::fmt_value(mins, sizeof mins, static_cast<float>((w.off_in_s + 59) / 60), 0, true);
        std::snprintf(info_line, sizeof info_line,
                      "http://192.168.4.1   %s joined   off in %s min", n, mins);
    } else if (w.state == wifi::State::Failed) {
        std::snprintf(info_line, sizeof info_line, "the C6 would not start the hotspot");
    } else if (w.state == wifi::State::UpdatingC6) {
        std::snprintf(info_line, sizeof info_line,
                      "giving the C6 Wi-Fi firmware; the dash restarts after");
    }
    wifi_join_.set_text(join_line);
    wifi_info_.set_text(info_line);
}

bool UpdateScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;
    if (e.button != Button::E || e.press != Press::Short) return false;
    // `ota` decides whether it can; this only asks. A refusal comes back on
    // the status line rather than being second-guessed here.
    static constexpr core::state::CommandId kIds[kActions] = {
        core::state::CommandId::OtaInstallSd,
        core::state::CommandId::OtaBootDevOnce,
        core::state::CommandId::OtaPromote,
        core::state::CommandId::WifiToggle,
    };
    return post(core::state::Command(kIds[focus_ < kActions ? focus_ : 0]));
}

void UpdateScreen::draw_background(Canvas& c) {
    c.draw_text(14, kUpdRunning.y, "Running", theme::secondary, 1, Align::Left);
    c.draw_text(14, kUpdOther.y, "Other slot", theme::secondary, 1, Align::Left);
    c.draw_text(14, kUpdC6.y, "C6", theme::secondary, 1, Align::Left);
    c.draw_text(14, kUpdNoteY, "Parked only: not moving, engine off. Card file: update/dash.bin",
                theme::secondary, 1, Align::Left);
    c.draw_text(14, static_cast<int16_t>(kUpdNoteY + 14),
                "USB: idf.py flash = new stable, tools/flash_dev.py = dev slot",
                theme::secondary, 1, Align::Left);
    c.draw_text(14, static_cast<int16_t>(kUpdNoteY + 28),
                "Wi-Fi: upload dash.bin, download logs and firmware from the page",
                theme::secondary, 1, Align::Left);
}

WidgetList UpdateScreen::widgets() { return WidgetList{list_, 7}; }

}  // namespace ui
