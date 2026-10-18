// Engine data, from two sources that must not be confused.
//
// `IEngine` is the ECU. The dash cannot talk to it yet: the P4 board has
// no CAN transceiver, and the K-line adapter's CANH/CANL run through an
// SN65HVD230 so those wires cannot be repurposed as a UART without a respin
// (§10.1). `NullEngine` is therefore what ships today, and every screen
// already renders `--` for an offline ECU.
//
// `IEngineSense` is the inductive pickup on TACH_SIG (GPIO21), the neutral switch and the
// two wheel sensors. It is always available and owes nothing to the ECU.
// Keep it that way even after the adapter works, so a dead adapter never
// blanks the rider's RPM.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

struct EngineData {
    float rpm       = 0.0f;
    float tps_pct   = 0.0f;
    float batt_v    = 0.0f;
    bool  mil       = false;    // check-engine lamp
};

class IEngine {
public:
    virtual ~IEngine() = default;
    virtual bool online() const = 0;
    virtual bool read(EngineData& out) = 0;    // false when there is no data
};

// What is compiled in today. UartEngine, CanEngine and BleEngine arrive
// later and nothing above this line changes when they do — which is the
// entire reason this interface exists before any of them.
class NullEngine final : public IEngine {
public:
    bool online() const override { return false; }
    bool read(EngineData&) override { return false; }
};

// Free-running edge counters. `ctrl` reads these at 500 Hz and does the
// maths; the driver does no smoothing and no unit conversion, because the
// tach-edge-to-shift-lamp path has a 3 ms budget and nothing here may spend
// it.
class IEngineSense {
public:
    virtual ~IEngineSense() = default;
    virtual Status init() = 0;

    struct Counts {
        uint32_t tach_pulses  = 0;   // totals since boot, not deltas
        uint32_t front_pulses = 0;
        uint32_t rear_pulses  = 0;

        // When the most recent edge on each channel arrived, timestamped in
        // the ISR rather than at read time.
        //
        // This is not a nicety. `ctrl` polls at 500 Hz, and at 6000 rpm the
        // pulses are 10 ms apart, so four ticks out of five see no edge at
        // all. Timing them by the polling interval instead of by the edges
        // reads 30000 rpm on an engine doing 6000. The counter says how
        // many; these say when.
        uint64_t tach_edge_us  = 0;
        uint64_t front_edge_us = 0;
        uint64_t rear_edge_us  = 0;

        uint64_t t_us         = 0;   // when the set was latched together
        bool     neutral      = false;
    };

    // One latched read, so the counts and the timestamp agree. Reading them
    // separately at 500 Hz is how you get an RPM spike every few seconds.
    //
    // Neutral reads LOW on the wire — opto-isolated, pulled up by R64 on GPIO12. The
    // driver inverts it here so nothing above ever has to know that.
    virtual Status read(Counts& out) = 0;
};

}  // namespace hal
