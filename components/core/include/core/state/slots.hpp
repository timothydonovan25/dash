// The slot table and the per-frame snapshot of it.
//
// This is the whole shared-state surface of the firmware. If a task needs a
// number from another task, it comes from here and from nowhere else. There
// is no `extern` anywhere in this codebase, and this file is why.
#pragma once
#include "core/state/published.hpp"
#include "core/state/vehicle.hpp"

namespace core::state {

// One writer each. The comment on each slot names its owner; that is the
// contract, and it is the only thing keeping the seqlock honest.
struct Slots {
    Published<Engine>   engine;     // ctrl
    Published<Motion>   motion;     // ctrl
    Published<Attitude> attitude;   // imu
    Published<Gnss>     gnss;       // gnss
    Published<Trip>     trip;       // logic
    Published<Lap>      lap;        // logic
    Published<Elec>     elec;       // keeper
    Published<Media>    media;      // ble (not in this build)
    Published<Tpms>     tpms;       // ble (not in this build)
    Published<Health>   health;     // keeper
};

// One coherent read of everything, taken once per frame. Screens get this
// and never touch a slot themselves — that is rule 4, and it is what makes
// a screen a pure function you can test with a literal.
struct Frame {
    Engine   engine;
    Motion   motion;
    Attitude att;
    Gnss     gnss;
    Trip     trip;
    Lap      lap;
    Elec     elec;
    Media    media;
    Tpms     tpms;
    Health   health;
    uint32_t t_ms = 0;
};

// Ten seqlock reads. Each slot is internally consistent; the frame as a
// whole is not an instant, and does not need to be — the newest lean angle
// and a 40 ms old lap time on the same screen is not a bug.
Frame snapshot(const Slots& s, uint32_t t_ms);

}  // namespace core::state
