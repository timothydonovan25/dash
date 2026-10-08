#include "services/settings_store.hpp"

namespace services {

namespace cfg = core::config;

size_t load_settings(hal::IStorage& storage, cfg::Settings& s) {
    size_t n = 0;
    for (size_t i = 0; i < cfg::item_count(); ++i) {
        const cfg::ConfigItem& it = cfg::items()[i];
        if (it.kind == cfg::Kind::Action) continue;
        float v = 0.0f;
        if (hal::ok(storage.read_f32(hal::kNsCfg, it.key, v))) {
            cfg::set(s, it, v);      // clamps on the way in
            ++n;
        }
    }
    return n;
}

size_t SettingsSaver::save_changed(hal::IStorage& storage, const cfg::Settings& now) {
    if (!valid_) {
        saved_ = now;
        valid_ = true;
        return 0;
    }
    // Through the table rather than memcmp, so padding bytes and fields
    // nobody can edit never cause a spurious write.
    size_t n = 0;
    for (size_t i = 0; i < cfg::item_count(); ++i) {
        const cfg::ConfigItem& it = cfg::items()[i];
        if (it.kind == cfg::Kind::Action) continue;
        const float v = cfg::get(now, it);
        if (v == cfg::get(saved_, it)) continue;
        if (hal::ok(storage.write_f32(hal::kNsCfg, it.key, v))) {
            cfg::set(saved_, it, v);
            ++n;
        }
    }
    return n;
}

}  // namespace services
