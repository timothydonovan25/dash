// NVS behind hal::IStorage.
//
// `life` and `cfg` stay in separate namespaces: losing the settings must not
// cost the rider their odometer, and the two are written at very different
// rates. The odometer is a double for the reason core/trip spells out — a
// float stops counting at 100,000 miles.
#pragma once
#include "hal/i_storage.hpp"

namespace drivers {

class NvsStorage final : public hal::IStorage {
public:
    hal::Status init() override;
    hal::Status read_f32(const char* ns, uint16_t key, float& out) override;
    hal::Status write_f32(const char* ns, uint16_t key, float value) override;
    hal::Status read_f64(const char* ns, uint16_t key, double& out) override;
    hal::Status write_f64(const char* ns, uint16_t key, double value) override;
    hal::Status commit() override;

private:
    bool ready_ = false;
};

}  // namespace drivers
