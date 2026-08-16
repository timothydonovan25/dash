#include "ui/screens.hpp"
#include "ui/chrome.hpp"

namespace ui {

using core::state::Frame;
namespace cfg = core::config;

// The whole of CONFIG mode is this one screen rendering one table (§13).
// Adding a setting is a line in `kItems` — no screen changes, no field
// arrays, no switch statement here.
namespace {
constexpr Rect kCfgTable {14,  40, 452, 250};
constexpr Rect kCfgHint  {14, 298, 452,  14};
}  // namespace

void ConfigScreen::enter(Canvas&) {
    table_.set_bounds(kCfgTable);
    table_.configure(10, 2);
    hint_.set_bounds(kCfgHint);
    hint_.configure("C/D move   E edit", color::kDim, 1, Align::Left);
    list_[0] = &table_;
    list_[1] = &hint_;
}

uint8_t ConfigScreen::focus_count() const {
    const size_t n = cfg::item_count();
    return n > 255 ? 255 : static_cast<uint8_t>(n);
}

void ConfigScreen::update(const Frame&) {
    if (!settings_) {
        table_.clear_rows();
        table_.set_row(0, "no settings bound", "--");
        table_.set_row_count(1);
        return;
    }

    // The table scrolls itself around the focused row, so only a window of
    // it is ever built.
    const size_t total = cfg::item_count();
    const size_t visible = Table::kRows;
    size_t first = 0;
    if (focus_ >= visible) first = focus_ - visible + 1;

    size_t rows = 0;
    for (size_t i = 0; i < visible && first + i < total; ++i) {
        const cfg::ConfigItem& it = cfg::items()[first + i];

        // The row being edited carries a '>' in front of its value. The
        // highlight alone says which row the cursor is on; it does not say
        // whether C and D will move the cursor or change the number, and
        // those are the two things a rider needs to tell apart.
        char value[28];
        size_t vk = 0;
        if (editing_ && first + i == focus_) {
            value[vk++] = '>';
            value[vk++] = ' ';
        }
        cfg::format(*settings_, it, value + vk, sizeof value - vk);

        // The group is shown on the row so a scrolled list still says which
        // section you are in, without a header that scrolls away.
        char label[24];
        size_t k = 0;
        for (const char* p = it.label; *p && k + 1 < sizeof label; ++p) label[k++] = *p;
        label[k] = '\0';

        table_.set_row(i, label, value);
        ++rows;
    }
    table_.set_row_count(rows);
    table_.set_focus(static_cast<int>(focus_ - first));

    hint_.set_text(editing_ ? "C/D change   E done" : "C/D move   E edit");
}

bool ConfigScreen::handle(const core::nav::InputEvent& e) {
    using core::nav::Button;
    using core::nav::Press;

    if (!settings_) return false;
    const size_t total = cfg::item_count();
    if (total == 0) return false;
    if (focus_ >= total) focus_ = 0;

    const cfg::ConfigItem& it = cfg::items()[focus_];

    if (e.button == Button::E) {
        if (it.kind == cfg::Kind::Action) {
            // An action item posts its command and never touches Settings.
            return post(core::state::Command(it.action));
        }
        editing_ = !editing_;
        return true;
    }

    // C and D move the cursor normally, and change the value while editing.
    // nav only forwards them here when the screen claims focus, which is
    // what `focus_count()` is for.
    if (e.button == Button::C || e.button == Button::D) {
        if (!editing_) return false;            // let nav move the focus
        // C is up and D is down, on the value as well as on the list. They
        // were the other way round here, so pressing up counted a setting
        // down, which is the sort of thing you notice once and then swear at
        // every time.
        const int dir = (e.button == Button::C) ? +1 : -1;
        if (cfg::nudge(*settings_, it, dir)) {
            // Report it up so the keeper writes NVS and every module holding
            // a copy of this value refreshes. The screen does not write
            // flash and does not reach into the shift-light config itself.
            post(core::state::Command::config_set(it.key,
                                                  cfg::get(*settings_, it)));
        }
        return true;
    }

    if (e.press == Press::Long && e.button == Button::B && editing_) {
        editing_ = false;
        return true;
    }
    return false;
}

void ConfigScreen::draw_background(Canvas& c) {
    chrome::title(c, "CONFIG");
    c.draw_text(static_cast<int16_t>(kScreenW - 14), chrome::kTitleY,
                "saved automatically", theme::secondary, 1, Align::Right);
}

WidgetList ConfigScreen::widgets() { return WidgetList{list_, 2}; }

}  // namespace ui
