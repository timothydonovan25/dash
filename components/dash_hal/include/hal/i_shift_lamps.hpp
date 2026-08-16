// The eight shift lamps and the master gate that feeds them.
//
// Two separate pieces of hardware, behind one interface because they are
// useless apart. The lamp bits are GPA0..GPA7 on the MCP23S17; the current
// for all eight passes through a low-side FET (Q11) gated by IO16. With that
// gate off, every lamp is dark no matter what the expander says — which is
// exactly the bug this interface exists to make hard to repeat.
//
// The gate is also the brightness control, and it has to be: eight lamps
// plus the backlight plus the 3V3 rail is about 610 mA, against the 500 mA
// a USB 2.0 port will give you. On the bench, with no engine running, the
// duty is clamped (`bsp::kShiftDutyUsbMax`) so the board does not brown out
// the expander that holds the panel's reset line.
#pragma once
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

class IShiftLamps {
public:
    virtual ~IShiftLamps() = default;
    virtual Status init() = 0;

    // Bit 0 is lamp 1, straight from core::shift::pattern().
    virtual Status set_mask(uint8_t mask) = 0;

    // 0 turns everything off however the mask is set. This is the only way
    // the lamps are ever lit.
    virtual Status set_master(uint8_t percent) = 0;
};

}  // namespace hal
