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

// Exposed for the tests, and useful on its own: NMEA carries position as
// ddmm.mmmm, which is degrees and minutes jammed together. Reading it as a
// decimal number puts you a few hundred miles away.
double degrees_from_ddmm(const char* s, char hemi);

}  // namespace core::nmea
