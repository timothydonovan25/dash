// DIAG -> Doom on the bike (doom/doom.hpp): Doom's 320x200 scaled 1.5x to
// 480x300 on the panel, letterboxed, from the pod's held buttons, and only
// while the bike is still (core::flash::still: wheels and GPS stopped, the
// engine at most idling). The pod has the buttons, so there is no legend.
#pragma once
#include "doom/doom.hpp"
#include "services/services.hpp"

namespace wiring {

class DoomBoard final : public doom::Board {
public:
    explicit DoomBoard(services::Deps& d) : d_(d) {}

    void begin() override;
    void frame(const uint16_t* rgb565) override;
    uint8_t held() const override { return d_.buttons_held.load(std::memory_order_relaxed); }
    bool parked() const override { return services::still_for_flash(d_); }

private:
    services::Deps& d_;
    uint16_t* band_ = nullptr;   // one blit's worth, in PSRAM, from begin()
};

}  // namespace wiring
