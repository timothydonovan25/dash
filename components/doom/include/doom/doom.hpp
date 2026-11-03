// DIAG -> Doom on either board: doomgeneric (components/doomgeneric) run off
// the SD card, on the panel, from the five buttons. The board supplies what
// differs between them (doom::Board): how a picture reaches its panel, which
// buttons are held, and whether the bike is parked.
//
// E on the menu row starts Doom, or resumes it. launch() runs on render's
// task between frames, and from then until active() goes false the panel is
// Doom's and render draws nothing. Doom gives it back three ways, none of
// them a reset:
//   - Quit Game in Doom's menu (I_Quit -> DG_Quit): paused where it was,
//     options saved; DIAG -> Doom carries on from there.
//   - the bike no longer parked (Board::parked()): paused the same way.
//   - a fatal error (every exit() in Doom is DG_Exit): Doom's task stops for
//     good and the row reads "error" until the next power-on. Doom's state
//     cannot be rebuilt in place, so a game is started once per power-on.
// It starts or resumes only parked; otherwise the row reads "parked only".
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
#include "hal/i_storage.hpp"
#include "ui/screen.hpp"

#include <atomic>
#include <cstdint>

namespace doom {

// Doom's own picture, what Board::frame() gets.
inline constexpr int kWidth = 320;
inline constexpr int kHeight = 200;

class Board {
public:
    virtual ~Board() = default;
    // The panel is Doom's from here, at the start and at every resume:
    // clear it and draw whatever frames the picture.
    virtual void begin() = 0;
    // One picture, kWidth x kHeight RGB565, row by row.
    virtual void frame(const uint16_t* rgb565) = 0;
    // The five buttons as held right now, bit 0 = A.
    virtual uint8_t held() const = 0;
    // May Doom have the panel? Turning false pauses a game in progress.
    virtual bool parked() const = 0;
};

class Program final : public ui::IProgram {
public:
    // Doom's task runs on `core` at `priority`, below everything that must
    // not wait for it: the renderer is CPU-bound.
    Program(hal::ICard& card, Board& board, int core, unsigned priority)
        : card_(card), board_(board), core_(core), priority_(priority) {}

    const char* name() const override { return "Doom"; }
    // "ready", "paused", "parked only", "no WAD", "no card" or "error". The
    // card is looked at most every 2 s: the menu asks every frame, and each
    // look is a directory listing.
    const char* state() const override;
    bool launch() override;
    bool active() const override { return active_.load(); }

    // On Doom's task, for the hooks in doom.cpp.
    Board& board() { return board_; }
    const char* wad_path() const { return wad_path_; }
    // Give the panel back and wait for launch() to resume.
    void pause();
    // Give the panel back for good.
    [[noreturn]] void stop();

private:
    bool find_wad() const;   // fills wad_path_; false if games/ has none

    hal::ICard&       card_;
    Board&            board_;
    const int         core_;
    const unsigned    priority_;
    std::atomic<bool> active_{false};
    std::atomic<bool> dead_{false};
    void*             task_ = nullptr;   // Doom's TaskHandle_t, once started

    mutable char        wad_path_[64] = {};
    mutable const char* card_state_ = "no card";
    mutable uint32_t    card_state_ms_ = 0;
    mutable bool        card_state_known_ = false;
};

}  // namespace doom
