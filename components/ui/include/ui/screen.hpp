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
    // The logger's card totals since boot, for keeper's 5 s console block.
    uint32_t log_bytes     = 0;
    uint32_t log_errors    = 0;
    uint32_t log_max_ms    = 0;   // slowest write
    uint32_t gnss_sentences= 0;
    uint32_t gnss_bad_csum = 0;
    uint16_t render_hz_x10 = 0;
    uint16_t render_sent_hz = 0;   // of those frames, the ones that sent anything
    uint16_t ctrl_late     = 0;    // ctrl ticks that missed their deadline
    // I2C0 bus recoveries, and how many of those failed to free SDA. The
    // difference is a pod that glitches versus a cable that is shorted.
    uint16_t pod_recoveries= 0;
    uint16_t pod_stuck     = 0;
    // Times the imu task fell back to polling because no data-ready edge
    // arrived. Climbing at the sample rate means INT1 is not working.
    uint32_t imu_timeouts  = 0;
    // Most samples the imu task found waiting in one wake, since boot. The
    // FIFO holds about 128; near that, an SD card stall nearly lost data.
    uint16_t imu_catchup_max = 0;
    // Wakes that found more than 64 / more than 100 waiting, since boot.
    uint32_t imu_catchup_over64 = 0;
    uint32_t imu_catchup_over100 = 0;
    // When render pushed its first whole frame, 0 until then. The start-up
    // sweep is timed from it, so `ctrl`'s lamps and the screen's tach sweep
    // together even though ctrl starts first.
    uint32_t first_frame_ms = 0;
    // CONFIG's Eject has unmounted the card and it is still in the socket:
    // safe to pull. Cleared when it comes out. Written by `storage`.
    bool     sd_ejected     = false;
    // The card is the laptop's, as a USB drive (drivers::UsbDrive): no
    // logging, no console. Written by `storage`.
    bool     sd_usb         = false;
    // The P4's die temperature, from `keeper` once a second. Valid only
    // with chip_temp_ok: there is no number that means "not read".
    float    chip_temp_c    = 0.0f;
    bool     chip_temp_ok   = false;
    // The task whose stack has come closest to full, and how close: its
    // high-water mark in bytes (services::watch_stacks, once a second).
    char     stack_low_task[12] = {};
    uint32_t stack_low_free_b   = 0;
    // A core dump in flash, from `keeper` at boot: the task that crashed
    // ("?" if the dump's summary would not read), empty with none. A dump
    // stays until the next crash overwrites it, so `crash_new` says whether
    // it is this boot's: the reset was a panic or a watchdog. Read it over
    // USB with `idf.py coredump-info`.
    char     crash_task[16] = {};
    bool     crash_new      = false;
};

// A program DIAG's menu can start that is not a screen: it takes the panel
// over from the render task, so nothing about it is drawn from a Frame. Only
// a build that has one sets Context::program (Doom, doom/doom.hpp).
class IProgram {
public:
    virtual ~IProgram() = default;
    // The menu row's label, and its one-word reading ("ready", "no WAD").
    virtual const char* name() const = 0;
    virtual const char* state() const = 0;
    // Asked from render's on_input, between frames: the program has the
    // panel from here. False if it cannot start (nothing to run, not
    // parked), and the menu stays as it was.
    virtual bool launch() = 0;
    // While true the panel is the program's: render draws nothing and drops
    // presses. When it turns false the glass is unknown and render repaints.
    virtual bool active() const = 0;
};

struct Context {
    const core::config::Settings* settings = nullptr;
    ICommandSink*                 commands = nullptr;
    const SystemInfo*             system   = nullptr;
    IProgram*                     program  = nullptr;
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

    // Whether the App should draw the mode/screen title bar above this
    // screen. The big-speed screen says no: it is the one a rider stares at
    // for hours and it wants every pixel.
    virtual bool wants_title() const { return true; }

    // True if the screen consumed the event. C, D and E reach the screen
    // only after nav has decided they are not navigation.
    virtual bool handle(const core::nav::InputEvent&) { return false; }

    virtual WidgetList widgets() = 0;

    // A name for widget `i`, for diagnostics — the wire-cost report names
    // the widget eating the frame budget rather than pointing at an index.
    // Optional: a screen that has not bothered gets `w0`, `w1` and so on.
    virtual const char* widget_name(size_t) const { return nullptr; }

    // How many focusable rows this screen has right now. nav needs it to
    // wrap the cursor, and it changes with the data on some screens.
    virtual uint8_t focus_count() const { return 0; }
    virtual void set_focus(uint8_t) {}
    // A screen that moves its own cursor -- CONFIG does, opening and closing
    // a submenu -- says where it put it, so nav's copy follows. -1: never.
    virtual int focus_now() const { return -1; }

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
