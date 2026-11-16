// NMEA 0183 sentence parsing. Pure logic, byte in / state out.
//
// This lives in core, not in the PA1616D driver, and that split is the whole
// point: the driver's job becomes "read the UART, hand over bytes", and
// every parsing bug — the ones that actually happen, like a fix that reads
// 4230.0 degrees north because ddmm.mmmm was treated as decimal degrees —
// is reproducible on the host against a captured log.
//
// Fed one character at a time so it works directly from a UART event with no
// intermediate buffer and no assumption that a read ends on a line boundary.
#pragma once
#include <cstdint>
#include "core/state/vehicle.hpp"

namespace core::nmea {

enum class Result : uint8_t {
    Incomplete = 0,   // still mid-sentence
    Updated,          // a sentence completed and changed the fix
    Ignored,          // a complete sentence this parser does not use
    BadChecksum,      // arrived corrupt; the caller may want to count these
    Malformed,        // right shape, wrong contents
};

class Parser {
public:
    // Feed one byte. When this returns Updated, `out` has been modified.
    Result feed(char ch, state::Gnss& out);

    // Statistics for the DIAG GPS screen. A receiver with a marginal
    // connection shows up here as a rising checksum-error count long before
    // it shows up as a lost fix.
    uint32_t sentences() const  { return sentences_; }
    uint32_t bad_checksum() const { return bad_checksum_; }
    uint32_t overruns() const   { return overruns_; }

private:
    Result end_of_sentence(state::Gnss& out);
    Result parse_rmc(state::Gnss& out);
    Result parse_gga(state::Gnss& out);
    Result parse_gsv(const char* talker, state::Gnss& out);

    // GSV, per talker (GP, GL, ...): the set being read and the last whole
    // one. A set is several sentences of four satellites each.
    static constexpr uint8_t kTalkers = 4;
    static constexpr uint8_t kSkyMax = 24;
    struct Sky {
        char    talker[2] = {};
        uint8_t view = 0;
        uint8_t n = 0;
        uint8_t snr[kSkyMax] = {};
    };
    Sky reading_[kTalkers];
    Sky done_[kTalkers];

    // The longest sentence a consumer receiver emits is well under 100
    // characters. 128 leaves room and bounds the memory: nothing here
    // allocates.
    static constexpr uint16_t kMaxLine = 128;
    static constexpr uint8_t  kMaxFields = 24;

    char     line_[kMaxLine] = {};
    uint16_t len_ = 0;
    bool     in_sentence_ = false;
    bool     overrun_ = false;

    // Field slices into line_, filled in once the sentence completes.
    const char* field_[kMaxFields] = {};
    uint8_t     fields_ = 0;

    uint32_t sentences_ = 0;
    uint32_t bad_checksum_ = 0;
    uint32_t overruns_ = 0;
};

// Whole sentences with a good checksum, for the receiver's bring-up
// (drivers/pa1616d.cpp): it listens for any real sentence at a baud rate,
// and for the PMTK001 answers to its commands. Not the Parser, which keeps
// only RMC and GGA and needs a fix to write into.
class LineReader {
public:
    // Feed one byte. True when a sentence has just ended with a checksum
    // that matches; line() is then its body, between '$' and '*'.
    bool feed(char ch);
    const char* line() const { return line_; }

private:
    static constexpr uint16_t kMax = 128;
    char     line_[kMax] = {};
    uint16_t len_ = 0;
    bool     in_ = false;
    bool     over_ = false;
};

// The flag of `PMTK001,<cmd>,<flag>` (the receiver's acknowledgement) when
// `line` answers `cmd`, else -1. Flags: 0 invalid, 1 unsupported, 2 valid
// but failed, 3 done (GlobalTop PMTK command packet, packet 001).
int ack_flag(const char* line, uint16_t cmd);

// Exposed for the tests, and useful on its own: NMEA carries position as
// ddmm.mmmm, which is degrees and minutes jammed together. Reading it as a
// decimal number puts you a few hundred miles away.
double degrees_from_ddmm(const char* s, char hemi);

// A receiver that stops sending -- unplugged, browned out, a dead UART --
// leaves its last sentence's `fix` set, and the slot would go on offering
// that position and speed forever: the speedo's GPS fallback froze on it,
// and a finish line or waypoint set then landed where the bike had been.
// So a fix with no new one for this long is no fix. Long enough for a
// receiver still at its 1 Hz power-on rate (the 10 Hz command not taken).
// `logic`'s lap timer keeps its own, shorter, silence check.
inline constexpr uint32_t kFixMaxAgeMs = 2500;

// Clears `fix` if the last fix (`rx_ms`) is older than kFixMaxAgeMs.
// True if it did, which is a change to publish.
inline bool age_fix(state::Gnss& g, uint32_t now_ms) {
    if (!g.fix || static_cast<int32_t>(now_ms - g.rx_ms) <= static_cast<int32_t>(kFixMaxAgeMs)) {
        return false;
    }
    g.fix = false;
    return true;
}

}  // namespace core::nmea
