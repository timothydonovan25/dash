#include "core/nmea/nmea.hpp"
#include <cstddef>

namespace core::nmea {
namespace {

constexpr float kMphPerKnot = 1.15077945f;

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool empty(const char* s) { return s == nullptr || s[0] == '\0'; }

// Small fixed-point readers. strtod would work but drags in locale handling
// and, on some newlib builds, a surprising amount of code.
double read_double(const char* s, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (empty(s)) return 0.0;
    bool neg = false;
    if (*s == '-') { neg = true; ++s; }
    else if (*s == '+') ++s;

    double v = 0.0;
    bool any = false;
    while (is_digit(*s)) { v = v * 10.0 + (*s - '0'); ++s; any = true; }
    if (*s == '.') {
        ++s;
        double scale = 0.1;
        while (is_digit(*s)) { v += (*s - '0') * scale; scale *= 0.1; ++s; any = true; }
    }
    if (!any || *s != '\0') return 0.0;      // trailing junk: not a number
    if (ok) *ok = true;
    return neg ? -v : v;
}

uint32_t read_uint(const char* s, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (empty(s)) return 0;
    uint32_t v = 0;
    bool any = false;
    while (is_digit(*s)) { v = v * 10 + static_cast<uint32_t>(*s - '0'); ++s; any = true; }
    if (!any || *s != '\0') return 0;
    if (ok) *ok = true;
    return v;
}

// hhmmss.sss -> milliseconds since midnight UTC.
uint32_t read_time_ms(const char* s, bool* ok) {
    *ok = false;
    if (empty(s)) return 0;
    // Need at least hhmmss.
    for (int i = 0; i < 6; ++i) if (!is_digit(s[i])) return 0;
    const uint32_t h = static_cast<uint32_t>((s[0] - '0') * 10 + (s[1] - '0'));
    const uint32_t m = static_cast<uint32_t>((s[2] - '0') * 10 + (s[3] - '0'));
    const uint32_t sec = static_cast<uint32_t>((s[4] - '0') * 10 + (s[5] - '0'));
    if (h > 23 || m > 59 || sec > 60) return 0;

    uint32_t frac_ms = 0;
    if (s[6] == '.') {
        double scale = 100.0;
        for (int i = 7; is_digit(s[i]) && scale >= 1.0; ++i) {
            frac_ms += static_cast<uint32_t>((s[i] - '0') * scale);
            scale /= 10.0;
        }
    }
    *ok = true;
    return ((h * 3600 + m * 60 + sec) * 1000) + frac_ms;
}

}  // namespace

double degrees_from_ddmm(const char* s, char hemi) {
    if (empty(s)) return 0.0;

    // Degrees are everything before the last two digits ahead of the point:
    // 4230.1234 is 42° 30.1234', and 12245.6789 is 122° 45.6789'. Reading
    // either as a plain decimal lands you in a different state.
    const char* dot = s;
    while (*dot && *dot != '.') ++dot;
    const size_t int_digits = static_cast<size_t>(dot - s);
    if (int_digits < 3) return 0.0;                 // not a ddmm field

    const size_t deg_digits = int_digits - 2;
    double deg = 0.0;
    for (size_t i = 0; i < deg_digits; ++i) {
        if (!is_digit(s[i])) return 0.0;
        deg = deg * 10.0 + (s[i] - '0');
    }
    bool ok = false;
    const double minutes = read_double(s + deg_digits, &ok);
    if (!ok) return 0.0;
    if (minutes >= 60.0) return 0.0;

    double out = deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') out = -out;
    return out;
}

Result Parser::feed(char ch, state::Gnss& out) {
    if (ch == '$') {
        // A '$' mid-sentence means the previous one was cut short. Start
        // over rather than trying to salvage it.
        in_sentence_ = true;
        len_ = 0;
        overrun_ = false;
        return Result::Incomplete;
    }
    if (!in_sentence_) return Result::Incomplete;

    if (ch == '\r' || ch == '\n') {
        in_sentence_ = false;
        if (len_ == 0) return Result::Incomplete;
        if (overrun_) {
            ++overruns_;
            return Result::Malformed;
        }
        line_[len_] = '\0';
        return end_of_sentence(out);
    }

    if (len_ + 1 >= kMaxLine) {
        overrun_ = true;      // keep consuming to the line end, then drop it
        return Result::Incomplete;
    }
    line_[len_++] = ch;
    return Result::Incomplete;
}

Result Parser::end_of_sentence(state::Gnss& out) {
    // Split off the checksum.
    uint16_t star = 0;
    bool have_star = false;
    for (uint16_t i = 0; i < len_; ++i) {
        if (line_[i] == '*') { star = i; have_star = true; break; }
    }
    if (!have_star || star + 2 >= len_) return Result::Malformed;

    const int hi = hex_value(line_[star + 1]);
    const int lo = hex_value(line_[star + 2]);
    if (hi < 0 || lo < 0) return Result::Malformed;

    uint8_t sum = 0;
    for (uint16_t i = 0; i < star; ++i) sum ^= static_cast<uint8_t>(line_[i]);
    if (sum != static_cast<uint8_t>(hi * 16 + lo)) {
        ++bad_checksum_;
        return Result::BadChecksum;
    }
    line_[star] = '\0';

    // Split fields in place.
    fields_ = 0;
    field_[fields_++] = line_;
    for (uint16_t i = 0; i < star && fields_ < kMaxFields; ++i) {
        if (line_[i] == ',') {
            line_[i] = '\0';
            field_[fields_++] = &line_[i + 1];
        }
    }
    ++sentences_;

    // Talker id is two characters and varies with the constellation in use
    // — GP, GN, GL. Match on the sentence type only.
    const char* type = field_[0];
    size_t type_len = 0;
    while (type[type_len]) ++type_len;
    if (type_len < 5) return Result::Ignored;
    const char* kind = type + 2;

    if (kind[0] == 'R' && kind[1] == 'M' && kind[2] == 'C') return parse_rmc(out);
    if (kind[0] == 'G' && kind[1] == 'G' && kind[2] == 'A') return parse_gga(out);
    return Result::Ignored;
}

Result Parser::parse_rmc(state::Gnss& out) {
    // $xxRMC,time,status,lat,N,lon,E,knots,course,date,magvar,magvarE*cs
    if (fields_ < 10) return Result::Malformed;

    const bool valid = field_[2][0] == 'A';

    bool ok = false;
    const uint32_t t = read_time_ms(field_[1], &ok);
    if (ok) out.utc_ms = t;

    if (!valid) {
        // A void fix still carries the time, which is worth keeping — but
        // the position in it is stale or zero and must not be published.
        out.fix = false;
        return Result::Updated;
    }

    const double lat = degrees_from_ddmm(field_[3], field_[4][0]);
    const double lon = degrees_from_ddmm(field_[5], field_[6][0]);
    if (lat == 0.0 && lon == 0.0) return Result::Malformed;   // null island

    out.fix = true;
    out.lat = lat;
    out.lon = lon;

    bool sok = false;
    const double knots = read_double(field_[7], &sok);
    if (sok) out.speed_mph = static_cast<float>(knots) * kMphPerKnot;

    bool cok = false;
    const double course = read_double(field_[8], &cok);
    // A stationary receiver emits an empty course field rather than a zero.
    // Overwriting the last good heading with 0 would swing the map north
    // every time the bike stops at a light.
    if (cok) out.course_deg = static_cast<float>(course);

    return Result::Updated;
}

Result Parser::parse_gga(state::Gnss& out) {
    // $xxGGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,geoid,M,age,ref*cs
    if (fields_ < 10) return Result::Malformed;

    bool qok = false, ok = false;
    const uint32_t q = read_uint(field_[6], &qok);
    if (qok) out.quality = static_cast<uint8_t>(q);

    const uint32_t sats = read_uint(field_[7], &ok);
    if (ok) out.sats = static_cast<uint8_t>(sats);

    const double hdop = read_double(field_[8], &ok);
    if (ok) out.hdop = static_cast<float>(hdop);

    const double alt = read_double(field_[9], &ok);
    if (ok) out.alt_m = static_cast<float>(alt);

    // Quality 0 means no fix, whatever the position field says. Only acted
    // on when the field actually parsed — a mangled field is not a report
    // that the fix was lost.
    if (qok && q == 0) out.fix = false;

    return Result::Updated;
}

}  // namespace core::nmea
