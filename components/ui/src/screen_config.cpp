#include "ui/screens.hpp"
#include "ui/chrome.hpp"

#include <cstdio>

namespace ui {

using core::state::Frame;
using core::state::CommandId;
namespace cfg = core::config;

// The whole of CONFIG mode is this one screen rendering one table (§13).
// Adding a setting is a line in `kItems` — no screen changes, no field
// arrays, no switch statement here.
//
// It opens on a short list of sections (core::config's kSections), each with
// a live one-line summary. E opens one: its items under a heading per group.
// B goes back. Every row says what it is now, an action included, so a
// press always shows an answer.
namespace {
constexpr Rect kCfgTable {14,  32, 452, 260};
constexpr Rect kCfgHint  {14, 300, 452,  12};
constexpr const char* kLead = "CONFIG/";
constexpr uint8_t kVisible = 10;
// How long an action's row says "..." waiting for what it shows to change,
// before it gives up and shows it anyway. A TPMS learn listens for a minute.
constexpr uint32_t kRanMs = 4000;
constexpr uint32_t kLearnMs = 60000;
// An action with nothing to show says "Done" this long.
constexpr uint32_t kDoneMs = 1500;

void copy(char* dst, size_t cap, const char* src) {
    size_t k = 0;
    for (const char* p = src; p && *p && k + 1 < cap; ++p) dst[k++] = *p;
    dst[k] = '\0';
}

void upper(char* dst, size_t cap, const char* src) {
    size_t k = 0;
    for (const char* p = src; p && *p && k + 1 < cap; ++p)
        dst[k++] = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 32) : *p;
    dst[k] = '\0';
}

bool eq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void fmt_key(const cfg::Settings& s, uint16_t key, char* out, size_t n) {
    const cfg::ConfigItem* it = cfg::find(key);
    if (it) cfg::format(s, *it, out, n);
    else copy(out, n, "");
}

const char* card_state(const Frame& f, const SystemInfo* sys) {
    if (sys && sys->sd_usb) return "On laptop";
    if (sys && sys->sd_ejected) return "Safe to pull";
    switch (f.health[core::state::Dev::Sd].state) {
        case core::state::DevState::Ok:     return "Card ok";
        case core::state::DevState::Absent: return "No card";
        case core::state::DevState::Init:   return "Mounting";
        default:                            return "Card fault";
    }
}

const char* wifi_state(const Frame& f) {
    switch (f.wifi.state) {
        case core::wifi::State::Off:        return "Off";
        case core::wifi::State::Starting:   return "Starting";
        case core::wifi::State::On:         return "On";
        case core::wifi::State::Failed:     return "Failed";
        case core::wifi::State::UpdatingC6: return "Updating radio";
    }
    return "";
}

// What an action's row shows when nothing is pending: the state it acts on.
// Empty for one with nothing to show (it says "Done" once run).
void action_state(const cfg::ConfigItem& it, const Frame& f, const SystemInfo* sys,
                  const cfg::Settings& s, char* out, size_t n) {
    out[0] = '\0';
    switch (it.action) {
        case CommandId::StorageEject:
            copy(out, n, card_state(f, sys));
            break;
        case CommandId::StorageToHost:
            copy(out, n, sys && sys->sd_usb ? "On laptop" : "Parked only");
            break;
        case CommandId::WifiToggle:
            if (f.wifi.state == core::wifi::State::On && f.wifi.clients)
                std::snprintf(out, n, "On, %u joined", static_cast<unsigned>(f.wifi.clients));
            else
                copy(out, n, wifi_state(f));
            break;
        case CommandId::BlePairingMode:
            if (f.ble_diag.pairing_s)
                std::snprintf(out, n, "Open %us", static_cast<unsigned>(f.ble_diag.pairing_s));
            else
                copy(out, n, "Closed");
            break;
        case CommandId::BleForgetPhone:
            if (f.ble_diag.bonds)
                std::snprintf(out, n, "%u paired", static_cast<unsigned>(f.ble_diag.bonds));
            else
                copy(out, n, "None paired");
            break;
        case CommandId::LapSetLine:
            if (f.lap.phase != core::lap::Phase::NoLine) copy(out, n, "Line set");
            else copy(out, n, f.gnss.fix ? "No line" : "Needs GPS");
            break;
        case CommandId::LapClear:
            std::snprintf(out, n, "%u laps", static_cast<unsigned>(f.lap.count));
            break;
        case CommandId::TripReset:
            std::snprintf(out, n, "%.1f mi", static_cast<double>(f.trip.trip_a_mi));
            break;
        case CommandId::ServiceDone: {
            if (s.service_every_h <= 0.0f) { copy(out, n, "Off"); break; }
            const float left = s.service_at_h + s.service_every_h - f.trip.eng_hours;
            if (left <= 0.0f) copy(out, n, "Due now");
            else std::snprintf(out, n, "Due in %.0f h", static_cast<double>(left));
            break;
        }
        case CommandId::ImuCalibrate:
            std::snprintf(out, n, "%.1f/%.1f deg", static_cast<double>(s.imu_roll_offset_deg),
                          static_cast<double>(s.imu_pitch_offset_deg));
            break;
        case CommandId::TpmsLearnFront:
            copy(out, n, s.tpms_front_id ? (f.tpms.front.valid ? "Heard" : "Bound") : "No sensor");
            break;
        case CommandId::TpmsLearnRear:
            copy(out, n, s.tpms_rear_id ? (f.tpms.rear.valid ? "Heard" : "Bound") : "No sensor");
            break;
        default:
            break;
    }
}

// The right-hand side of a section's row on the top level: how it stands.
void section_summary(const char* name, const Frame& f, const SystemInfo* sys,
                     const cfg::Settings& s, char* out, size_t n) {
    char a[12], b[12];
    out[0] = '\0';
    if (eq(name, "Display")) {
        fmt_key(s, 0x0101, a, sizeof a);
        fmt_key(s, 0x0102, b, sizeof b);
        std::snprintf(out, n, "%s, %s", a, b);
    } else if (eq(name, "Shift lights")) {
        std::snprintf(out, n, "from %u rpm", static_cast<unsigned>(s.shift_base_rpm));
    } else if (eq(name, "Ride")) {
        fmt_key(s, 0x0901, a, sizeof a);
        std::snprintf(out, n, "log %s", a);
    } else if (eq(name, "Phone")) {
        if (f.health.ok(core::state::Dev::Phone)) copy(out, n, "Connected");
        else if (f.ble_diag.pairing_s) std::snprintf(out, n, "Pairing %us", static_cast<unsigned>(f.ble_diag.pairing_s));
        else if (f.ble_diag.bonds) std::snprintf(out, n, "%u paired", static_cast<unsigned>(f.ble_diag.bonds));
        else copy(out, n, "Not paired");
    } else if (eq(name, "Bike")) {
        if (s.service_every_h > 0.0f) {
            const float left = s.service_at_h + s.service_every_h - f.trip.eng_hours;
            if (left <= 0.0f) copy(out, n, "service due");
            else std::snprintf(out, n, "service %.0f h", static_cast<double>(left));
        } else {
            std::snprintf(out, n, "%u gears", static_cast<unsigned>(s.gear_count));
        }
    } else if (eq(name, "Sensors")) {
        const unsigned tp = (s.tpms_front_id ? 1u : 0u) + (s.tpms_rear_id ? 1u : 0u);
        std::snprintf(out, n, "TPMS %u/2, ECU %s", tp, s.ecu_on ? "on" : "off");
    } else if (eq(name, "Card & Wi-Fi")) {
        const char* card = card_state(f, sys);
        if (eq(card, "Safe to pull")) card = "Ejected";
        std::snprintf(out, n, "%s, Wi-Fi %s", card,
                      f.wifi.state == core::wifi::State::Off ? "off" : "on");
    } else if (eq(name, "Developer")) {
        copy(out, n, s.show_fps ? "FPS on" : "");
    }
}
}  // namespace

void ConfigScreen::enter(Canvas& c) {
    table_.set_bounds(kCfgTable);
    table_.configure(kVisible, 2);
    hint_.set_bounds(kCfgHint);
    hint_.configure("", color::kDim, 1, Align::Left);
    const int16_t lead_w = static_cast<int16_t>(c.text_width(kLead, 1));
    crumb_.set_bounds(Rect{static_cast<int16_t>(14 + lead_w), chrome::kTitleY, 240, 9});
    crumb_.configure("", theme::primary, 1, Align::Left);
    where_.set_bounds(Rect{346, chrome::kTitleY, 120, 9});
    where_.configure("", theme::secondary, 1, Align::Right);
    list_[0] = &table_;
    list_[1] = &hint_;
    list_[2] = &crumb_;
    list_[3] = &where_;
    first_ = 0;
    if (resume_) {
        resume_ = false;
        return;
    }
    in_group_ = false;
    editing_ = false;
    focus_ = 0;
}

uint8_t ConfigScreen::focus_count() const {
    const size_t n = in_group_ ? cfg::section_size(group_) : cfg::section_count();
    return n > 255 ? 255 : static_cast<uint8_t>(n);
}

void ConfigScreen::update(const Frame& f) {
    if (!settings_) {
        table_.clear_rows();
        table_.set_row(0, "no settings bound", "--");
        table_.set_row_count(1);
        return;
    }
    const SystemInfo* sys = has_ctx() ? ctx().system : nullptr;

    char where[16];
    std::snprintf(where, sizeof where, "%u/%u", static_cast<unsigned>(focus_ + 1),
                  static_cast<unsigned>(focus_count()));
    where_.set_text(where);

    if (in_group_) {
        section_rows(f);
        return;
    }

    crumb_.set_text("SETTINGS");
    const size_t total = cfg::section_count();
    if (focus_ < first_) first_ = focus_;
    if (focus_ >= first_ + kVisible) first_ = static_cast<uint8_t>(focus_ - kVisible + 1);
    size_t rows = 0;
    for (size_t i = 0; i < kVisible && first_ + i < total; ++i) {
        char value[24];
        section_summary(cfg::section_name(first_ + i), f, sys, *settings_, value, sizeof value);
        table_.set_header(i, false);
        table_.set_row(i, cfg::section_name(first_ + i), value);
        ++rows;
    }
    table_.set_row_count(rows);
    table_.set_focus(static_cast<int>(focus_ - first_));
    table_.set_editing(false);
    hint_.set_text("C/D move   E open   changes save themselves");
}

// One section: a heading wherever the group changes (if it has more than
// one), then its items. The cursor counts items; the rows count both.
void ConfigScreen::section_rows(const Frame& f) {
    const SystemInfo* sys = has_ctx() ? ctx().system : nullptr;
    char crumb[24];
    upper(crumb, sizeof crumb, cfg::section_name(group_));
    crumb_.set_text(crumb);

    // The rows: item index, or -1 for a heading (its group is the next item's).
    constexpr size_t kMaxRows = 64;
    int16_t row_item[kMaxRows];
    size_t n_rows = 0, focus_row = 0;
    const size_t total = cfg::section_size(group_);
    const bool headings = cfg::section_groups(group_) > 1;
    for (size_t i = 0; i < total && n_rows + 2 <= kMaxRows; ++i) {
        const cfg::ConfigItem* it = cfg::section_item(group_, i);
        if (headings && (i == 0 || !eq(it->group, cfg::section_item(group_, i - 1)->group)))
            row_item[n_rows++] = -1;
        if (i == focus_) focus_row = n_rows;
        row_item[n_rows++] = static_cast<int16_t>(i);
    }

    // Scroll only when the cursor would leave the view, and bring its
    // heading along when moving up onto a group's first item.
    size_t first = first_;
    if (focus_row < first) first = focus_row;
    if (first > 0 && focus_row == first && row_item[first - 1] < 0) --first;
    if (focus_row >= first + kVisible) first = focus_row - kVisible + 1;
    if (first + kVisible > n_rows) first = n_rows > kVisible ? n_rows - kVisible : 0;
    first_ = static_cast<uint8_t>(first);

    // An action run a moment ago: stamp it, and drop it once settled.
    if (ran_key_ && ran_ms_ == 0) ran_ms_ = f.t_ms ? f.t_ms : 1;

    size_t rows = 0;
    for (size_t r = first_; r < n_rows && rows < kVisible; ++r, ++rows) {
        if (row_item[r] < 0) {
            if (rows + 1 == kVisible) break;   // a heading with nothing under it
            const cfg::ConfigItem* next = cfg::section_item(group_, static_cast<size_t>(row_item[r + 1]));
            char head[24];
            upper(head, sizeof head, next->group);
            table_.set_header(rows, true);
            table_.set_row(rows, head, "");
            continue;
        }
        const size_t idx = static_cast<size_t>(row_item[r]);
        const cfg::ConfigItem& it = *cfg::section_item(group_, idx);
        const bool here = idx == focus_;
        char label[24];
        char value[24];
        copy(label, sizeof label, it.label);

        if (it.kind == cfg::Kind::Action) {
            char now[20];
            action_state(it, f, sys, *settings_, now, sizeof now);
            if (here) focus_state_.set(now);
            bool pending = false;
            if (ran_key_ == it.key) {
                const bool learn = it.action == CommandId::TpmsLearnFront ||
                                   it.action == CommandId::TpmsLearnRear;
                const uint32_t age = f.t_ms - ran_ms_;
                // With a state: until it changes (or gives up). Without: "Done", briefly.
                const bool unchanged = eq(now, ran_before_.s);
                pending = now[0] ? unchanged && age < (learn ? kLearnMs : kRanMs)
                                 : age < kDoneMs;
                if (!pending) ran_key_ = 0;
                else copy(value, sizeof value, now[0] ? (learn ? "Listening..." : "...") : "Done");
            }
            if (armed_key_ == it.key) copy(value, sizeof value, "E again?");
            else if (!pending) copy(value, sizeof value, now[0] ? now : "Run");
        } else {
            char v[20];
            cfg::format(*settings_, it, v, sizeof v);
            if (editing_ && here) std::snprintf(value, sizeof value, "< %s >", v);
            else copy(value, sizeof value, v);
            // A '*' after the label: changed from the default, which a long
            // E puts back.
            if (cfg::get(*settings_, it) != cfg::default_value(it)) {
                size_t lk = 0;
                while (label[lk]) ++lk;
                copy(label + lk, sizeof label - lk, " *");
            }
        }
        table_.set_header(rows, false);
        table_.set_row(rows, label, value);
    }
    table_.set_row_count(rows);
    table_.set_focus(static_cast<int>(focus_row - first_));
    table_.set_editing(editing_);

    const cfg::ConfigItem* it = focus_ < total ? cfg::section_item(group_, focus_) : nullptr;
    const char* hint = "C/D move   E change   hold E default   B back";
    if (editing_) hint = "C/D change   E or B done";
    else if (it && it->kind == cfg::Kind::Bool) hint = "C/D move   E switch   hold E default   B back";
    else if (it && it->kind == cfg::Kind::Action)
        hint = armed_key_ == it->key ? "E again to confirm   any other button cancels"
             : it->confirm           ? "C/D move   E run (asks twice)   B back"
                                     : "C/D move   E run   B back";
    hint_.set_text(hint);
}

bool ConfigScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;

    if (!settings_) return false;

    // Any button but the second E forgets a half-confirmed action.
    const uint16_t armed = armed_key_;
    armed_key_ = 0;

    if (!in_group_) {
        if (e.button == Button::E && e.press == Press::Short) {
            if (focus_ >= cfg::section_count()) return false;
            group_ = focus_;
            in_group_ = true;
            focus_ = 0;
            first_ = 0;
            return true;
        }
        return false;
    }

    const size_t total = cfg::section_size(group_);
    if (total == 0) return false;
    if (focus_ >= total) focus_ = 0;
    const cfg::ConfigItem& it = *cfg::section_item(group_, focus_);

    // B leaves: the edit first if there is one, then the section, with the
    // cursor back on the section it came from. On the press: B has no long
    // meaning (core::input::Config::on_press_mask).
    if (e.button == Button::B) {
        if (editing_) {
            editing_ = false;
            has_pending_ = false;
        } else {
            in_group_ = false;
            focus_ = group_;
            first_ = 0;
        }
        return true;
    }

    // A long E puts the item back to its default (cfg::default_value).
    if (e.button == Button::E && e.press == Press::Long) {
        if (it.kind == cfg::Kind::Action) return true;
        editing_ = false;
        post_value(it, cfg::default_value(it));
        return true;
    }

    if (e.button == Button::E && e.press == Press::Short) {
        if (it.kind == cfg::Kind::Action) {
            // One that loses something wants E twice (ConfigItem::confirm).
            if (it.confirm && armed != it.key) {
                armed_key_ = it.key;
                return true;
            }
            // What the row shows now, so it can say "..." until that changes.
            ran_key_ = it.key;
            ran_ms_ = 0;
            ran_before_.set(focus_state_.s);
            // An action item posts its command and never touches Settings.
            return post(core::state::Command(it.action));
        }
        // A switch flips on the one press: nothing to choose between.
        if (it.kind == cfg::Kind::Bool) {
            post_value(it, current(it) > 0.5f ? 0.0f : 1.0f);
            return true;
        }
        editing_ = !editing_;
        has_pending_ = false;
        return true;
    }

    // C and D move the cursor normally, and change the value while editing.
    // nav only forwards them here when the screen claims focus, which is
    // what `focus_count()` is for.
    if (e.button == Button::C || e.button == Button::D) {
        if (!editing_) return false;            // let nav move the focus
        // C is up and D is down, on the value as well as on the list.
        const int dir = (e.button == Button::C) ? +1 : -1;
        const float from = current(it);
        const float to = cfg::nudged(it, from, dir);
        if (to != from) post_value(it, to);
        return true;
    }
    return false;
}

float ConfigScreen::current(const cfg::ConfigItem& it) {
    const float shown = cfg::get(*settings_, it);
    if (has_pending_ && (pending_key_ != it.key || pending_val_ == shown)) {
        has_pending_ = false;                // applied, or another item
    }
    return has_pending_ ? pending_val_ : shown;
}

void ConfigScreen::post_value(const cfg::ConfigItem& it, float v) {
    // Up to logic, which applies it; the keeper then writes NVS and every
    // module holding a copy refreshes. The screen does not write Settings,
    // flash, or the shift-light config itself.
    if (v == current(it)) return;
    post(core::state::Command::config_set(it.key, v));
    has_pending_ = true;
    pending_key_ = it.key;
    pending_val_ = v;
}

void ConfigScreen::draw_background(Canvas& c) {
    c.draw_text(14, chrome::kTitleY, kLead, theme::secondary, 1, Align::Left);
    c.fill_rect(Rect{14, 22, static_cast<int16_t>(kScreenW - 28), 1}, theme::rule);
}

WidgetList ConfigScreen::widgets() { return WidgetList{list_, 4}; }

}  // namespace ui
