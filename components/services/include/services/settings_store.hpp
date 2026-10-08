// Settings to and from NVS, through the ConfigItem table.
//
// Separate from `keeper` so it carries no board: the P4 demo build runs
// exactly this code against the same NVS layout, which is how a setting
// surviving a power cycle gets tested before the real board exists.
#pragma once
#include "core/config/config.hpp"
#include "hal/i_storage.hpp"

namespace services {

// Every value that has ever been saved comes back through the same table
// that the config screen renders and that validates the ranges. A key that
// is not in NVS keeps its default, which is what a fresh board and a newly
// added setting both look like. Returns how many values were read.
size_t load_settings(hal::IStorage& storage, core::config::Settings& s);

// A shadow copy of the settings as they were last written to flash. Diffing
// against it is how a changed setting reaches NVS: the config screen edits
// Settings in place, and nothing on a fast path ever writes flash.
class SettingsSaver {
public:
    // Writes every value that differs from the last write, without
    // committing. The first call only takes the baseline. Returns how many
    // values were written.
    size_t save_changed(hal::IStorage& storage, const core::config::Settings& now);

private:
    core::config::Settings saved_;
    bool                   valid_ = false;
};

}  // namespace services
