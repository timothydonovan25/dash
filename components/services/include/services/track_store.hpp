// Saved tracks between `logic` and the card (core::track).
//
// `logic` decides everything about a track and runs on core 0, where nothing
// may touch the filesystem; `storage` owns the card on core 1. This is the
// one hand-off between them: a single job at a time, and the box belongs to
// one side or the other, never both. `logic` fills a request while it is
// Free and passes it over; `storage` does the work and passes it back with
// the answer; `logic` reads that and frees it. Whoever does not hold the
// turn does not touch anything else in here, which keeps it one writer at a
// time without a lock -- and nothing on core 0 ever waits on the card,
// because a request still out is simply not answered yet.
//
// Nothing here may include an ESP header: the host tests and the simulator
// serve it with a fake card.
#pragma once
#include <atomic>
#include <cstdint>
#include "core/delta/delta.hpp"
#include "core/track/track.hpp"
#include "hal/i_storage.hpp"

namespace services {

struct TrackMailbox {
    enum class Turn : uint8_t { Free = 0, Posted, Answered };
    enum class Op : uint8_t {
        None = 0,
        List,      // every saved track's header: `lib`
        Save,      // `rec` and the reference `pts[0..n)` into `slot`
        Load,      // the reference lap of `slot` into `pts[0..n)`
        Forget,    // delete `slot`'s file
    };

    std::atomic<Turn> turn{Turn::Free};
    Op      op = Op::None;
    uint8_t slot = 0;
    // The answer: did the card do it. False is the card failing (busy, an
    // I/O error), worth asking again; a slot with nothing in it is ok, with
    // nothing in it.
    bool    ok = false;

    core::track::Library lib;
    core::track::Record  rec;
    // Line-relative (core::track::shift), in both directions.
    core::delta::Point   pts[core::delta::kMaxPoints];
    uint16_t             n = 0;

    // `storage`'s scratch for a whole file. Here rather than on its 4 kB
    // stack; the box lives in PSRAM on the board.
    uint8_t file[core::track::kMaxFileBytes];

    // `logic`: the box is Free, so a request may be written and posted.
    bool free() const { return turn.load(std::memory_order_acquire) == Turn::Free; }
    void post() { turn.store(Turn::Posted, std::memory_order_release); }
    bool answered() const { return turn.load(std::memory_order_acquire) == Turn::Answered; }
    void release() { turn.store(Turn::Free, std::memory_order_release); }
};

// `storage`'s side: does the posted job, if there is one, against `card`,
// and answers it. True if it did one. Never blocks for longer than the card
// calls themselves.
bool serve_tracks(TrackMailbox& box, hal::ICard& card);

}  // namespace services
