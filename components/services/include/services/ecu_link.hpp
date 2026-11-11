// The ECU, by radio: the K-line adapter's frames, forwarded by the C6 as
// ESP-Hosted custom messages (core/ecu has the format and the rules, and
// architecture §10.1 the reasons).
//
// The frames arrive on ESP-Hosted's RPC task, core 1. That task is the one
// writer: it feeds the Link and publishes its snapshot. `ctrl`, on core 0,
// reads the snapshot and judges its age itself, so a link that has gone
// quiet reads offline with nobody having to say so.
#pragma once
#include "core/ecu/ecu.hpp"
#include "core/state/published.hpp"
#include "hal/i_clock.hpp"
#include "hal/i_engine.hpp"

namespace services {

class EspNowEngine final : public hal::IEngine {
public:
    explicit EspNowEngine(const hal::IClock& clock) : clock_(clock) {}

    bool online() const override;
    bool read(hal::EngineData& out) override;
    // Registers for the C6's messages. Needs the ESP-Hosted link up.
    hal::Status attach() override;
    hal::EngineLink link() const override;

private:
    static void on_message(uint32_t id, const uint8_t* data, size_t len, void* self);

    const hal::IClock& clock_;
    core::ecu::Link link_;                           // the RPC task's
    core::Published<core::ecu::Snapshot> snap_;
};

}  // namespace services
