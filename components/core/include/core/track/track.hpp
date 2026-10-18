// Track memory: the finish line, the splits, the record and the reference
// lap of every track ridden, kept on the card and brought back when the bike
// turns up at one again. Pure logic.
//
// What identifies a track is where its finish line is. There is no track
// database and no name: a track is "the line the rider set here", and the
// next time a good fix lands within kRecallFt of it the line, its splits
// and its reference lap come back by themselves, so a second visit needs no
// setup at all. That is the thing the factory dashes charge for.
//
// Splits are kept with the line on purpose. They are placed by the first
// clean lap (core::lap::Sectors) and then stay where they are for every
// session after, which is what makes a sector best from last month mean
// anything today.
//
// The reference lap is the fastest clean lap ever ridden there, not the
// session's: the delta then says how today compares with the rider's best,
// from the first lap of the day.
//
// The file is this firmware's own and nobody else reads it: little-endian,
// fixed layout, a CRC over the header and another over the points, so a
// file cut short by a power cut is noticed and not believed.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/delta/delta.hpp"
#include "core/geo/geo.hpp"
#include "core/lap/sectors.hpp"

namespace core::track {

inline constexpr size_t kMaxTracks = 16;

// A good fix this close to a saved finish line brings the track back. A
// quarter mile or so: inside any paddock, and a finish line is never further
// than that from where a bike unloads. Two tracks closer than this to each
// other (a pee-wee loop beside the main track) still work: the nearest wins,
// and setting the line on the other one saves it as its own.
inline constexpr double kRecallFt = 1500.0;

// A line set this close to a saved one is that track's line moved, not a new
// track: the old record goes, because times over a different line are not
// comparable. Further away it is a new track.
inline constexpr double kSameTrackFt = 300.0;

struct Line {
    geo::LatLon at{};
    double      heading_deg = 0.0;
};

struct Record {
    bool     used = false;
    Line     finish{};
    bool     splits = false;                    // the split lines are placed
    Line     split[lap::kSectors - 1]{};
    uint32_t best_ms = 0;                       // fastest clean lap, 0 none
    uint32_t sector_best_ms[lap::kSectors] = {};
    // The lap time of the reference saved with it, 0 for none. Not always
    // `best_ms`: a reference lost to a bad file is replaced by the next
    // clean lap, which may be slower than the record.
    uint32_t ref_ms = 0;
    // GPS day (core::waypoint::gps_day) it was last ridden: which one goes
    // when every slot is full.
    uint32_t last_day = 0;
    uint16_t sessions = 0;                      // power-ons it was ridden in

    // The best sectors added up; 0 until each has one.
    uint32_t ideal_ms() const;
};

struct Library {
    Record rec[kMaxTracks]{};
};

// The saved track whose finish line is nearest `pos` and within `within_ft`,
// or -1.
int nearest(const Library& lib, geo::LatLon pos, double within_ft = kRecallFt);

// Where a line just set at `at` goes: the saved track it is a move of (within
// kSameTrackFt), else an empty slot, else the track ridden longest ago.
int slot_for_line(const Library& lib, geo::LatLon at);

// A fresh record for a line set at `at`: no splits, no bests.
Record fresh(const Line& finish, uint32_t day);

// A lap was finished. True if it beat the record, which it can only do
// clean: a lap with a fix gap in it may be two laps glued together.
bool offer_lap(Record& r, uint32_t lap_ms, bool clean);

// The session's sector bests, after a split: true if any beat the record.
// `session_best` is core::lap::Sectors::best_ms(), 0 for none.
bool offer_sectors(Record& r, const uint32_t (&session_best)[lap::kSectors]);

// --- the reference lap's points ---------------------------------------
//
// Kept on the card relative to the finish line rather than to whatever fix
// the firmware's local frame happened to start at this power-on, so a file
// written today lines up with tomorrow's frame. The projection is the
// traces' (services::logic_tick's to_local_ft), and it is linear, so moving
// between the two frames is adding or subtracting where the line sits in
// the current one.
void shift(delta::Point* pts, size_t n, float dx_ft, float dy_ft);

// --- the file -----------------------------------------------------------

inline constexpr uint32_t kMagic = 0x4B525444u;   // "DTRK"
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kHeaderBytes = 128;
inline constexpr size_t kPointBytes = 12;
inline constexpr size_t kMaxFileBytes =
    kHeaderBytes + delta::kMaxPoints * kPointBytes + 4;

// The whole file into `out`. 0 if it will not fit, or there are more
// points than a reference may have.
size_t encode(const Record& r, const delta::Point* pts, size_t n,
              uint8_t* out, size_t cap);

// The header alone, enough to list the tracks: false if it is not a track
// file, or not one this firmware wrote, or its CRC is wrong. `n_points` is
// how many the file says follow.
bool decode_header(const uint8_t* buf, size_t len, Record& r, size_t& n_points);

// The whole file. A good header with bad points is still the track (the
// line and the record), with no reference: `n` is then 0 and this is true.
bool decode(const uint8_t* buf, size_t len, Record& r, delta::Point* pts,
            size_t cap, size_t& n);

// Where slot `i` lives on the card, and its write-then-rename partner.
// False for a slot that does not exist.
bool file_name(char* out, size_t cap, size_t slot, bool temp = false);
inline constexpr const char* kDir = "tracks";

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);

}  // namespace core::track
