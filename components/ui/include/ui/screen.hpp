// The Screen interface (§7.3).
//
// A screen is a pure function of one Frame. It does not read sensors, does
// not touch a bus, and never calls get() on a slot. It receives a Frame and
// sets widget values — `update()` may not draw. The render task repaints
// whatever went dirty.
//
// That split is what keeps a screen file readable: a list of rectangles, a
// list of widgets, and a list of assignments. It is also what makes every
// screen in this firmware testable on a laptop against a literal Frame,
// which matters when the panel is still at the fab.
#pragma once
#include <cstdint>
#include "ui/widgets.hpp"
#include "core/state/slots.hpp"
#include "core/state/command.hpp"
#include "core/config/config.hpp"
#include "core/nav/nav.hpp"

namespace ui {

// A screen never writes state. It posts a command and the logic task decides
// (§4.5). This is the seam.
class ICommandSink {
public:
    virtual ~ICommandSink() = default;
    virtual bool post(const core::state::Command& c) = 0;
};

// Numbers that only the platform can know, filled in by the keeper task.
// Kept here rather than read from an esp_ header so `ui` stays host-buildable.
struct SystemInfo {
    uint32_t heap_free_b   = 0;
    uint32_t heap_min_b    = 0;
    uint32_t psram_free_b  = 0;
    uint32_t uptime_s      = 0;
    uint32_t log_records   = 0;
    uint32_t log_dropped   = 0;
    uint32_t gnss_sentences= 0;
    uint32_t gnss_bad_csum = 0;
    uint16_t render_hz_x10 = 0;
    uint16_t ctrl_late     = 0;    // ctrl ticks that missed their deadline
};

struct Context {
    const core::config::Settings* settings = nullptr;
    ICommandSink*                 commands = nullptr;
    const SystemInfo*             system   = nullptr;
};

class Screen {
public:
    virtual ~Screen() = default;

    virtual const char* name() const = 0;

    // Build widgets and lay them out. Called once when the screen becomes
    // visible; the canvas is passed so a screen can size text against the
    // font it will actually be drawn with.
    virtual void enter(Canvas&) {}
    virtual void leave() {}

    // Set widget values. Must not draw.
    virtual void update(const core::state::Frame& f) = 0;

    // The parts of a screen that are not widgets: frames, gauges, rules,
    // small print. Drawn once per full repaint, underneath the widgets.
    //
    // A screen draws its own chrome rather than owning a widget per line
    // because the chrome *is* the screen — it does not change, it is not
    // reused, and turning every rounded rectangle into a Widget would
    // triple the file for nothing.
    virtual void draw_background(Canvas&) {}

    // True if the screen consumed the event. C, D and E reach the screen
    // only after nav has decided they are not navigation.
    virtual bool handle(const core::nav::InputEvent&) { return false; }

    virtual WidgetList widgets() = 0;

    // How many focusable rows this screen has right now. nav needs it to
    // wrap the cursor, and it changes with the data on some screens.
    virtual uint8_t focus_count() const { return 0; }
    virtual void set_focus(uint8_t) {}

    void bind(const Context& ctx) { ctx_ = &ctx; }

protected:
    const Context& ctx() const { return *ctx_; }
    bool has_ctx() const { return ctx_ != nullptr; }

    core::units::System units() const {
        return has_ctx() && ctx_->settings
                   ? static_cast<core::units::System>(ctx_->settings->unit_system)
                   : core::units::System::Imperial;
    }

    bool post(const core::state::Command& c) {
        return has_ctx() && ctx_->commands ? ctx_->commands->post(c) : false;
    }

private:
    const Context* ctx_ = nullptr;
};

}  // namespace ui
