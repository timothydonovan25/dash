#include "services/ecu_link.hpp"

#include "esp_log.h"

extern "C" {
#include "esp_hosted_misc.h"   // no C++ guard of its own
}

namespace services {

namespace {
constexpr const char* kTag = "ecu";
}

void EspNowEngine::on_message(uint32_t, const uint8_t* data, size_t len, void* self) {
    auto& e = *static_cast<EspNowEngine*>(self);
    const bool was_latched = e.link_.snapshot().latched;
    e.link_.feed_forwarded(data, len, e.clock_.now_ms());
    const core::ecu::Snapshot& s = e.link_.snapshot();
    if (!was_latched && s.latched) {
        ESP_LOGI(kTag, "adapter %02x:%02x:%02x:%02x:%02x:%02x", s.addr[0], s.addr[1], s.addr[2],
                 s.addr[3], s.addr[4], s.addr[5]);
    }
    e.snap_.publish(s);
}

hal::Status EspNowEngine::attach() {
    const esp_err_t err = esp_hosted_register_custom_callback(core::ecu::kMsgId, on_message, this);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "no handler for the adapter's frames: %s", esp_err_to_name(err));
        return hal::Status::IoError;
    }
    return hal::Status::Ok;
}

bool EspNowEngine::online() const {
    core::ecu::Reading r;
    return core::ecu::reading(snap_.get(), r, clock_.now_ms());
}

bool EspNowEngine::read(hal::EngineData& out) {
    core::ecu::Reading r;
    if (!core::ecu::reading(snap_.get(), r, clock_.now_ms())) return false;
    out.rpm = r.rpm;
    out.tps_pct = r.tps_pct;
    out.batt_v = r.batt_v;
    out.engine_c = r.engine_c;
    out.intake_c = r.intake_c;
    out.map_kpa = r.map_kpa;
    out.inj_raw = r.inj_raw;
    return true;
}

hal::EngineLink EspNowEngine::link() const {
    const core::ecu::Snapshot s = snap_.get();
    hal::EngineLink l;
    l.adapter = core::ecu::adapter_online(s, clock_.now_ms());
    l.lost = s.lost;
    l.rejected = s.rejected + s.foreign;
    return l;
}

}  // namespace services
