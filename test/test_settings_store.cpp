#include "check.hpp"
#include "services/settings_store.hpp"

#include <map>
#include <string>

using core::config::Settings;

namespace {

// NVS as a map, keyed the way NVS keys are: namespace and item key.
class FakeNvs final : public hal::IStorage {
public:
    hal::Status init() override { return hal::Status::Ok; }
    hal::Status read_f32(const char* ns, uint16_t key, float& out) override {
        auto it = f32.find(id(ns, key));
        if (it == f32.end()) return hal::Status::NotPresent;
        out = it->second;
        return hal::Status::Ok;
    }
    hal::Status write_f32(const char* ns, uint16_t key, float v) override {
        f32[id(ns, key)] = v;
        ++writes;
        return hal::Status::Ok;
    }
    hal::Status read_f64(const char*, uint16_t, double&) override { return hal::Status::NotPresent; }
    hal::Status write_f64(const char*, uint16_t, double) override { return hal::Status::Ok; }
    hal::Status commit() override { return hal::Status::Ok; }

    std::map<std::string, float> f32;
    int writes = 0;

private:
    static std::string id(const char* ns, uint16_t key) {
        return std::string(ns) + ":" + std::to_string(key);
    }
};

}  // namespace

int main() {
    CASE("an empty NVS leaves every default alone");
    {
        FakeNvs nvs;
        Settings s;
        CHECK(services::load_settings(nvs, s) == 0);
        CHECK(s.brightness_pct == Settings{}.brightness_pct);
    }

    CASE("the first save only takes the baseline: nothing is written");
    {
        FakeNvs nvs;
        Settings s;
        services::SettingsSaver saver;
        CHECK(saver.save_changed(nvs, s) == 0);
        CHECK(nvs.writes == 0);
    }

    CASE("a changed setting is written once, and survives a reboot");
    {
        FakeNvs nvs;
        Settings s;
        services::SettingsSaver saver;
        saver.save_changed(nvs, s);
        s.sweep_ms = 3500;
        s.gauge_sweep = false;
        s.shift_brightness_pct = 40;
        CHECK(saver.save_changed(nvs, s) == 3);
        CHECK(saver.save_changed(nvs, s) == 0);       // nothing new

        Settings after_reboot;
        CHECK(services::load_settings(nvs, after_reboot) == 3);
        CHECK(after_reboot.sweep_ms == 3500);
        CHECK(!after_reboot.gauge_sweep);
        CHECK(after_reboot.shift_brightness_pct == 40);
    }

    CASE("a value out of range in flash is clamped on the way in");
    {
        FakeNvs nvs;
        nvs.write_f32(hal::kNsCfg, 0x0B02, 99999.0f);  // sweep time
        Settings s;
        services::load_settings(nvs, s);
        CHECK(s.sweep_ms == 5000);
    }

    return check::report("settings_store");
}
