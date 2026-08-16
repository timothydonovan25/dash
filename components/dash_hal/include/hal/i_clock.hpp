// Monotonic time.
//
// `core/` never calls this — core takes time as a parameter, which is what
// lets a lap timer be tested by handing it 30 seconds in a loop. Services
// call it, once per tick, and pass the number down.
#pragma once
#include <cstdint>

namespace hal {

class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t now_ms() const = 0;
    virtual uint64_t now_us() const = 0;
};

}  // namespace hal
