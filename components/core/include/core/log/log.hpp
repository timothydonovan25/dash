// The blackbox ring buffer (§9.2).
//
// Producers write fixed-size records and never touch the filesystem, never
// block, and never wait on the card. The `logger` task drains the ring to SD
// in big blocks. That split is the whole design: an SD card can stall for
// hundreds of milliseconds when its controller decides to do wear levelling,
// and `ctrl` has a 3 ms budget.
//
// So the contract here is deliberately lopsided: **a full buffer drops the
// newest record and counts it.** It does not block the producer, and it does
// not overwrite the oldest — a blackbox whose start is missing tells you
// nothing about how the incident began.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace core::log {

// One row of the blackbox. Fixed size on purpose: the drain writes whole
// records, and a variable-length format means a partial write at power-off
// corrupts the tail rather than truncating it.
struct Record {
    uint32_t t_ms       = 0;
    float    speed_mph  = 0.0f;
    float    rpm        = 0.0f;
    float    lean_deg   = 0.0f;
    float    pitch_deg  = 0.0f;
    float    g_lat      = 0.0f;   // lateral, RaceRender's Y
    float    g_lon      = 0.0f;   // accel and braking, RaceRender's X
    float    g_vert     = 0.0f;
    float    course_deg = 0.0f;
    double   lat        = 0.0;
    double   lon        = 0.0;
    uint32_t lap_ms     = 0;      // elapsed in the current lap
    // The time of a lap that finished on this tick, 0 otherwise. It rides
    // along in the record rather than going down a side channel so the
    // marker lands between the right two data rows: RaceRender takes lap
    // tags anywhere in the file but requires them in chronological order,
    // and the logger only ever sees the stream.
    uint32_t lap_done_ms= 0;
    uint16_t lap_num    = 0;      // 0-based, matching the lap tags
    uint16_t flags      = 0;
    uint8_t  sats       = 0;
    // 0 is neutral *and* unknown, because RaceRender has no third state.
    // Nothing is lost: kFlagNeutral is the sensed one, and this is inferred.
    uint8_t  gear       = 0;

    // The raw speed sources and the fused figure, so a ride can be replayed
    // through core/fusion on a laptop (tools/replay) and the filter tuned
    // against what the sensors really said. Log at 50 Hz for that.
    float    front_mph  = 0.0f;
    float    rear_mph   = 0.0f;
    float    gps_mph    = 0.0f;
    float    hdop       = 0.0f;
    uint32_t gps_utc_ms = 0;      // changes when a new fix arrives
    float    fused_mph  = 0.0f;
    float    fused_sigma= 0.0f;

    // GPS altitude, above mean sea level. Feet, as everything on this dash
    // is shown; the GPX export converts. No barometer, so it is the
    // receiver's own figure: good to a few tens of feet, noisy between
    // fixes, and meaningless without kFlagFix.
    float    alt_ft     = 0.0f;

    // The rider's heart rate from the watch (core/hr), bpm. 0 when there is
    // no current reading: the setting off, no watch, or the last one stale.
    uint16_t hr_bpm     = 0;
};

enum Flags : uint16_t {
    kFlagNeutral  = 1u << 0,
    kFlagAirborne = 1u << 1,
    kFlagFix      = 1u << 2,
    kFlagTiming   = 1u << 3,
    kFlagEcu      = 1u << 4,
};

// How often `logic` puts a row in the ring, as the config menu offers it.
//
// Only divisors of the 50 Hz logic tick are on the list. A rate the tick
// cannot divide evenly — 30 Hz, say — logs alternating 20 ms and 40 ms gaps,
// which RaceRender will happily read and which quietly stops the file being
// an honest time base. Picking from a list is also one button press to a
// known-good value, which is what you want with gloves on.
enum class Rate : uint8_t { Off = 0, Hz1, Hz5, Hz10, Hz25, Hz50 };

constexpr uint8_t kRateCount   = 6;
constexpr Rate    kDefaultRate = Rate::Hz10;

// The label and the interval come out of one table, so a menu option cannot
// disagree with the rate it selects. `kRateLabels` is what the ConfigItem
// enum points at — an array rather than an accessor, because the item table
// is a constexpr initialiser and a function call in it would drag the whole
// table out of flash and into RAM. The interval is 0 for Off.
extern const char* const kRateLabels[kRateCount];

uint32_t rate_interval_ms(uint8_t rate);

// Decides which ticks get a row. The logic task owns one, and it is the only
// thing in the firmware that knows a logging rate is not the tick rate.
class Decimator {
public:
    // `force` writes a row whatever the cadence. It is for the tick a lap
    // starts or finishes on: that record is the only channel the lap marker
    // has, so decimating it away loses the lap from the file entirely.
    //
    // It does not override Rate::Off. Off means no rows, including that one —
    // if the rider turned logging off, the firmware does not log anyway.
    bool due(uint8_t rate, uint32_t now_ms, bool force = false);

    // Forget the cadence, so the next tick logs. Called when the card is
    // swapped or the rate changes; a new file should start with a row.
    void reset() { primed_ = false; }

private:
    uint32_t last_ms_ = 0;
    bool     primed_  = false;
};

// When `logger` sends its RAM block to the card and when it commits it.
//
// An fsync on an SD card rewrites the file's data sector and its directory
// entry, and the SD-over-SPI driver spins through the card's busy time, on
// core 1 and holding SPI2 from the IMU. The logger used to do one whenever
// the ring was empty, which at 10 rows a second is between almost every row:
// about 4.5 small writes and syncs a second where one 8 kB block every few
// seconds was meant. This decides instead:
//
//  - Write: the block's oldest row is `max_age_ms` old. (A full block is
//    written by the logger anyway, as it fills.)
//  - WriteAndSync: something written has gone `sync_every_ms` uncommitted;
//    or the bike has just stopped, since key-off nearly always follows a
//    stop and the rows of the ride are then committed before it.
//
// The cost is what a sudden cut loses: rows not yet committed, up to about
// max_age_ms + sync_every_ms while moving. FAT stays consistent -- an
// unsynced file keeps its last committed length. `power` stops all writes at
// VIN cut, so nothing here could be saved at the cut itself.
struct FlushConfig {
    uint32_t max_age_ms    = 2000;
    uint32_t sync_every_ms = 5000;
};

enum class Flush : uint8_t { None, Write, WriteAndSync };

class FlushPolicy {
public:
    // A row went into the RAM block.
    void buffered(uint32_t now_ms);
    // The block went to the card (whole, or what there was of it).
    void written(uint32_t now_ms);
    // The file was committed.
    void synced();

    // What to do now. `stopped` is core::flash::still; its rising edge is a
    // sync. Call once per logger pass.
    Flush decide(uint32_t now_ms, bool stopped, const FlushConfig& c);

    bool has_rows() const { return has_rows_; }
    bool unsynced() const { return unsynced_; }

private:
    bool     has_rows_ = false;
    uint32_t oldest_ms_ = 0;
    bool     unsynced_ = false;
    uint32_t unsynced_since_ms_ = 0;
    bool     was_stopped_ = true;   // a dash that boots parked has nothing to commit
};

// Single producer, single consumer, lock-free. The producer is `logic`; the
// consumer is `logger` on the other core.
template <typename T, size_t N>
class Ring {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Ring<T> requires a trivially copyable T");
    static_assert(N >= 2, "Ring needs at least two slots");

public:
    // Producer only. False means the record was dropped because the drain
    // has fallen behind — which is information, not an error to retry.
    bool push(const T& v) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= N) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[w % N] = v;
        std::atomic_thread_fence(std::memory_order_release);
        write_.store(w + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer only.
    bool pop(T& out) {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        out = buf_[r % N];
        std::atomic_thread_fence(std::memory_order_acquire);
        read_.store(r + 1, std::memory_order_relaxed);
        return true;
    }

    // Drain into a caller's array. The logger writes 8 kB at a time rather
    // than a record at a time, because an SD card charges the same latency
    // for 512 bytes as for 8 kB.
    size_t pop_batch(T* out, size_t max) {
        size_t n = 0;
        while (n < max && pop(out[n])) ++n;
        return n;
    }

    size_t size() const {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_acquire);
    }
    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return N; }
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void clear() {
        read_.store(write_.load(std::memory_order_acquire),
                    std::memory_order_release);
    }

private:
    T                     buf_[N] = {};
    std::atomic<size_t>   write_{0};
    std::atomic<size_t>   read_{0};
    std::atomic<uint32_t> dropped_{0};
};

// A CSV line for one record, into a caller buffer. Written on the logger
// task, never on a producer. CSV rather than a packed binary format because
// the point of a blackbox you never open is nothing at all — this one opens
// in any spreadsheet on the card.
//
// The columns are named for RaceRender so the same file is both the
// blackbox and the video overlay source: `Time` in **seconds** (it reads
// that column as seconds, and milliseconds would import a six-minute
// session as a hundred hours), `X` and `Y` for the g axes, and the rest of
// its vocabulary where we have the data. The extra columns past those are
// ours; RaceRender ignores what it does not recognise and can drive custom
// gauges from them.
size_t format_csv(char* buf, size_t n, const Record& r);

// Two lines: the `# RaceRender Data` marker that has to be first in the
// file, then the column names.
size_t csv_header(char* buf, size_t n);

// `# Lap 3: 00:02:43.08` — a measured lap time, which beats RaceRender
// inferring one from the trace. `index` is 0-based, as its tags are.
size_t format_lap_marker(char* buf, size_t n, uint16_t index, uint32_t lap_ms);

// --- file names -----------------------------------------------------------
// A log starts life as logs/sNNN.csv, because at key-on there is no clock:
// no RTC on this board, and the GPS has not said what day it is. Once it
// has, the file is renamed to the local date with the same session number,
// logs/2026-10-13_s007.csv, so a card of rides sorts by when they were.

struct Date {
    uint16_t year  = 0;
    uint8_t  month = 0;
    uint8_t  day   = 0;
};

// The local calendar date at `utc_ms` into the UTC day `utc`, `tz_h` hours
// from UTC. An evening ride in the Americas is already tomorrow in UTC.
Date local_date(Date utc, uint32_t utc_ms, int32_t tz_h);

// "logs/sNNN.csv" before the date is known, "logs/YYYY-MM-DD_sNNN.csv"
// after; `date.year` 0 means not known. Returns the length, or 0 if it did
// not fit.
size_t log_file_name(char* buf, size_t n, Date date, unsigned session);

// The session number in a log's file name (no directory), either form, or
// -1 if it is not one. A new session takes one more than the highest on the
// card: renaming frees "sNNN.csv", so a free plain name is not enough. A
// detail file's name (core/log/detail.hpp, "s007_imu.csv") counts too.
int session_of(const char* name);

}  // namespace core::log
