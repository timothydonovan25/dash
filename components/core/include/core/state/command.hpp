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
    StorageEject,     // flush and unmount the card so it can be pulled
    ImuCalibrate,
    ImuSetMountOffset,
    MediaCtrl,        // i: 0 previous, 1 play/pause, 2 next — §11 "Ctrl"
    BleForgetPhone,   // drop every bond; the next phone pairs freely (§11.3)
    BlePairingMode,   // let one new phone bond, for core::ble::kPairingWindowMs
    OtaInstallSd,     // write update/dash.bin from the card into the dev slot
    OtaBootDevOnce,   // restart into dev; the next reset returns to stable
    OtaPromote,       // from dev: keep the running dev image as stable (§11.7)
    WifiToggle,       // the update hotspot on, or off if it is on
    NavDropWaypoint,  // i: which named waypoint (core::waypoint) to put here
    NavClearWaypoint, // i: which one to forget
    GearCalStart,     // open the calibration page (CONFIG -> Gearing)
    GearCalNext,      // E: start, move on once a gear is ready, accept
    GearCalSkip,      // long E: leave this gear out
    GearCalCancel,    // B: close it, discarding the run
    GearCalReset,     // the table back to its calibrated base
    LeanPeaksReset,   // IMU's short E: the session's lean peaks back to zero
    TrackForget,      // delete the saved track this line belongs to (core::track)
    PerfReset,        // PERF's E: forget the performance timers' bests
    StuntReset,       // STUNT's E: a new session score (core::stunt)
    CrashCancel,      // any button in the crash countdown; E once the SOS is out: "I'm OK"
    CrashTest,        // console `crash test`: the countdown now, as if after a crash
    NavGo,            // i: route to Frame::nav.dest[i] (core::route)
    NavCancel,        // stop routing
    NavAvoidFast,     // the 65 mph question: route without those roads
    NavKeepFast,      // ... or keep them
    NavSearch,        // i: 1 open "Where to?", 0 close it
    NavSearchKey,     // i: a character of core::route::kAlphabet to add, or 8 to delete one
    NavSearchPick,    // i: Frame::nav.hits[i]
    NavSearchGo,      // go to the street picked, at the number typed (none: the street)
    ShiftLampTest,    // CONFIG -> Shift -> Test lamps: `ctrl` runs the start-up sweep on the lamps
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
