// DIAG -> Doom on the demo board: doomgeneric (demo_p4/components/
// doomgeneric) run off the SD card, on the panel, from the five buttons.
//
// The menu row is a ui::IProgram. E on it only asks; the render task sees
// the request after its on_input, stops drawing, and calls start(), which
// hands the panel to a Doom task and never gives it back: the board is
// reset to leave. Everything else (BLE, Wi-Fi, ota, the bike task) carries
// on underneath, on the cores and priorities it had.
//
// The game's data is the first .wad in games/ on the card, put there from
// the Wi-Fi page (Deps::games). Its config and saves go in games/ too.
//
// Buttons, A..E top to bottom, as raw held state (not the Decoder's
// presses, which are made for menus and would turn a held "forward" into
// one step):
//   A forward / menu up      B back / menu down
//   C turn left              D turn right
//   E fire, menu select, "y" to a question
//   A+B together: the menu (Escape)    C+D together: use (doors, switches)
// A chord stands in for both its buttons, so A+B does not also walk.
#pragma once
#include "ui/screen.hpp"

#include <atomic>
#include <cstdint>

namespace drivers { class SdCard; }

namespace demo {

class P4Panel;

// The five buttons as held right now, bit 0 = A: touch zones and the pod
// OR'd together. One writer, the touch task, at 100 Hz.
struct RawButtons {
    std::atomic<uint8_t> mask{0};
};

class DoomProgram final : public ui::IProgram {
public:
    DoomProgram(drivers::SdCard& card, P4Panel& panel, const RawButtons& buttons)
        : card_(card), panel_(panel), buttons_(buttons) {}

    const char* name() const override { return "Doom"; }
    // "ready", "no WAD" or "no card". Looked up at most every 2 s: the menu
    // asks every frame, and each look is a directory listing on the card.
    const char* state() const override;
    bool launch() override;

    // Whether launch() has been asked; the render task polls this.
    bool requested() const { return requested_.load(); }
    // Called by the render task once it has stopped drawing. Starts the
    // Doom task, which owns the panel from here on.
    void start();

    // For the doomgeneric hooks in doom_program.cpp.
    P4Panel& panel() { return panel_; }
    const RawButtons& buttons() const { return buttons_; }
    const char* wad_path() const { return wad_path_; }

private:
    bool find_wad() const;   // fills wad_path_; false if games/ has none

    drivers::SdCard&   card_;
    P4Panel&           panel_;
    const RawButtons&  buttons_;
    std::atomic<bool>  requested_{false};

    mutable char       wad_path_[64] = {};
    mutable const char* state_ = "no card";
    mutable uint32_t   state_ms_ = 0;
    mutable bool       state_known_ = false;
};

}  // namespace demo
