// Persistent settings, life data, and the card.
//
// `life` (odometer, engine hours) and `cfg` (user settings) live in separate
// NVS namespaces, as they did in v1. Losing the settings must not cost the
// rider their odometer, and the two are written at very different rates.
//
// Keys are the `ConfigItem::key` numbers from core/config. They are stable
// forever — that is what lets an NVS partition written by an older firmware
// still load. Note that changing the partition table erases NVS, which is
// why §17 says set it before step 2 rather than after.
#pragma once
#include <cstddef>
#include <cstdint>
#include "hal/status.hpp"

namespace hal {

inline constexpr const char* kNsLife = "life";
inline constexpr const char* kNsCfg  = "cfg";

// Life keys. Not ConfigItems: the rider cannot edit these, and they are
// written by the keeper rather than the config screen.
inline constexpr uint16_t kKeyOdometerMi   = 0x0001;
inline constexpr uint16_t kKeyEngineHours  = 0x0002;
// Which panel FPC1 had at the last boot (drivers::PanelModel), so the probe
// tries it first. Written by wiring, at boot, only when it changes.
inline constexpr uint16_t kKeyPanelModel   = 0x0003;
// The named waypoints: three keys each from here (lat, lon, when), in
// core::waypoint's order.
inline constexpr uint16_t kKeyWaypointBase = 0x0100;

class IStorage {
public:
    virtual ~IStorage() = default;
    virtual Status init() = 0;

    // NotPresent for a key that has never been written — a fresh board, or
    // a setting added by a newer firmware. That is not an error, and the
    // caller keeps its default.
    virtual Status read_f32(const char* ns, uint16_t key, float& out) = 0;
    virtual Status write_f32(const char* ns, uint16_t key, float value) = 0;

    // The odometer is a double for the reason spelled out in core/trip: a
    // float stops counting at 100,000 miles.
    virtual Status read_f64(const char* ns, uint16_t key, double& out) = 0;
    virtual Status write_f64(const char* ns, uint16_t key, double value) = 0;

    virtual Status commit() = 0;
};

// The SD card: logs, sessions, tracks. Bulk and best-effort. Never on the
// real-time path, and never on core 0.
class ICard {
public:
    virtual ~ICard() = default;
    virtual Status init() = 0;
    virtual bool   present() const = 0;
    virtual Status append(const char* path, const void* data, size_t len) = 0;
    virtual Status sync() = 0;

    // Reading, for the firmware update (§11.7). Paths are relative to the
    // card's root, as for append. NotPresent for a missing file. Each read
    // stands alone -- nothing is held open between calls -- so a card pulled
    // mid-read is an error on the next call, not a dangling handle.
    virtual Status size_of(const char* path, size_t& out) = 0;
    virtual Status read_at(const char* path, size_t offset, void* buf,
                           size_t len, size_t& got) = 0;

    // Calls `fn` for each plain file in `dir`, with its name and size. For
    // the Wi-Fi page's log list. Without `sizes` the size is 0: each one is
    // a stat, a search of the whole directory, so a long listing with sizes
    // goes as its length squared (the logger's session pick needs names only).
    using ListFn = void (*)(const char* name, size_t size, void* ctx);
    virtual Status list(const char* dir, ListFn fn, void* ctx, bool sizes = true) = 0;

    // Deletes one file, for the Wi-Fi page. Busy for the file `append` is
    // writing: the logger still holds it open, and FAT would lose the rows.
    virtual Status remove(const char* path) = 0;

    // A whole small file, created or replaced, and closed again: a saved
    // track (core::track). Not for the log, which appends.
    virtual Status write_file(const char* path, const void* data, size_t len) = 0;

    // Fails rather than overwrite an existing `to`.
    virtual Status rename(const char* from, const char* to) = 0;

    // One piece of a file too big to hold whole, for the Wi-Fi page's game
    // upload: offset 0 creates or empties the file, any other offset must be
    // its current size. Each call opens and closes it, as read_at does.
    // Unsupported on a card that has no use for it.
    virtual Status write_at(const char* path, size_t offset, const void* data,
                            size_t len) {
        (void)path; (void)offset; (void)data; (void)len;
        return Status::Unsupported;
    }
};

}  // namespace hal
