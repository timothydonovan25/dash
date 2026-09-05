#include "core/state/slots.hpp"
#include "core/state/command.hpp"

namespace core::state {

Lap to_published(const lap::State& s, int32_t delta_ms, bool delta_valid) {
    Lap out;
    out.phase           = s.phase;
    out.count           = s.count;
    out.current_ms      = s.current_ms;
    out.last_ms         = s.last_ms;
    out.best_ms         = s.best_ms;
    out.delta_ms        = delta_ms;
    out.delta_valid     = delta_valid;
    out.dist_to_line_ft = s.dist_to_line_ft;
    return out;
}

Frame snapshot(const Slots& s, uint32_t t_ms) {
    Frame f;
    f.engine = s.engine.get();
    f.motion = s.motion.get();
    f.att    = s.attitude.get();
    f.gnss   = s.gnss.get();
    f.trip   = s.trip.get();
    f.lap    = s.lap.get();
    f.elec   = s.elec.get();
    f.media  = s.media.get();
    f.tpms   = s.tpms.get();
    f.health = s.health.get();
    f.ble_diag = s.ble_diag.get();
    f.t_ms   = t_ms;
    return f;
}

const char* dev_name(Dev d) {
    switch (d) {
        case Dev::Imu:      return "IMU";
        case Dev::Gnss:     return "GPS";
        case Dev::McpBoard: return "MCP board";
        case Dev::McpPod:   return "MCP pod";
        case Dev::Sd:       return "SD";
        case Dev::Ecu:      return "ECU";
        case Dev::Display:  return "Display";
        case Dev::BleHost:  return "BLE host";
        case Dev::Phone:    return "Phone";
        case Dev::TpmsF:    return "TPMS front";
        case Dev::TpmsR:    return "TPMS rear";
        case Dev::Count:    break;
    }
    return "?";
}

const char* command_name(CommandId id) {
    switch (id) {
        case CommandId::None:              return "none";
        case CommandId::LapSetLine:        return "lap.set_line";
        case CommandId::LapArm:            return "lap.arm";
        case CommandId::LapClear:          return "lap.clear";
        case CommandId::TripReset:         return "trip.reset";
        case CommandId::ConfigSet:         return "config.set";
        case CommandId::LogStart:          return "log.start";
        case CommandId::LogStop:           return "log.stop";
        case CommandId::StorageToHost:     return "storage.to_host";
        case CommandId::ImuCalibrate:      return "imu.calibrate";
        case CommandId::ImuSetMountOffset: return "imu.set_mount_offset";
        case CommandId::MediaCtrl:         return "media.ctrl";
    }
    return "?";
}

}  // namespace core::state
