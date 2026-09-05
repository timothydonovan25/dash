// Commands going down (§4.5).
//
// The UI never writes state. It posts one of these, and the logic side of
// the sensor hub drains them at 50 Hz. One queue, one drain point, so every
// command that ever changed anything can be logged in one place.
//
// The queue itself is a FreeRTOS object and lives in `services`. This header
// is only the message, so screens can build commands on the host.
#pragma once
#include <cstdint>

namespace core::state {

enum class CommandId : uint8_t {
    None = 0,
    LapSetLine,       // arg unused: the logic task uses the current fix
    LapArm,
    LapClear,
    TripReset,        // i: 0 = trip A, 1 = trip B
    ConfigSet,        // cfg: key + value
    LogStart,
    LogStop,
    StorageToHost,    // hand the SD card to USB MSC
    ImuCalibrate,
    ImuSetMountOffset,
    MediaCtrl,        // i: 0 previous, 1 play/pause, 2 next — §11 "Ctrl"
};

struct Command {
    CommandId id = CommandId::None;
    union {
        float   f;
        int32_t i;
        struct {
            uint16_t key;
            float    val;
        } cfg;
    };

    Command() : id(CommandId::None), i(0) {}
    explicit Command(CommandId c) : id(c), i(0) {}
    Command(CommandId c, int32_t v) : id(c), i(v) {}
    Command(CommandId c, float v) : id(c), f(v) {}
    static Command config_set(uint16_t key, float val) {
        Command c(CommandId::ConfigSet);
        c.cfg.key = key;
        c.cfg.val = val;
        return c;
    }
};

const char* command_name(CommandId id);

}  // namespace core::state
