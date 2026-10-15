#include "check.hpp"
#include "core/nmea/nmea.hpp"
#include <cstdio>
#include <cstring>

using namespace core::nmea;
using core::state::Gnss;

namespace {

Result feed_all(Parser& p, Gnss& g, const char* s) {
    Result r = Result::Incomplete;
    for (const char* c = s; *c; ++c) {
        const Result step = p.feed(*c, g);
        if (step != Result::Incomplete) r = step;
    }
    return r;
}

// Build a sentence from its body and feed it. The checksum is computed here
// so the test data stays readable — a hand-written one would only ever test
// that the parser agrees with a typo.
Result send(Parser& p, Gnss& g, const char* body) {
    uint8_t sum = 0;
    for (const char* c = body; *c; ++c) sum ^= static_cast<uint8_t>(*c);
    char line[160];
    std::snprintf(line, sizeof line, "$%s*%02X\r\n", body, sum);
    return feed_all(p, g, line);
}

const char* kGoodRmc =
    "GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W";
const char* kGoodGga =
    "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";

}  // namespace

int main() {
    CASE("ddmm.mmmm is degrees and minutes, not a decimal");
    {
        // The bug this exists to prevent: reading 4230.1234 as 4230.12
        // degrees, or as 42.30 degrees. Both are wrong, and only one of them
        // looks obviously wrong on a map.
        CHECK_NEAR(degrees_from_ddmm("4230.0000", 'N'), 42.5, 1e-9);
        CHECK_NEAR(degrees_from_ddmm("4230.0000", 'S'), -42.5, 1e-9);
        CHECK_NEAR(degrees_from_ddmm("12245.6789", 'W'), -122.761315, 1e-6);
        CHECK_NEAR(degrees_from_ddmm("00100.5000", 'E'), 1.008333333, 1e-8);
        CHECK_NEAR(degrees_from_ddmm("", 'N'), 0.0, 1e-12);
        CHECK_NEAR(degrees_from_ddmm("12", 'N'), 0.0, 1e-12);        // too short
        CHECK_NEAR(degrees_from_ddmm("4270.0000", 'N'), 0.0, 1e-12); // 70 minutes
    }

    CASE("the checksum of a real sentence validates");
    {
        // The textbook GPRMC, with the checksum it actually ships with. If
        // the XOR range is wrong — including the '$', or the '*' — this is
        // the case that catches it.
        Parser p;
        Gnss g;
        const Result r = feed_all(p, g,
            "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n");
        CHECK(r == Result::Updated);
        CHECK(p.bad_checksum() == 0);
    }

    CASE("a good RMC gives position, speed and course");
    {
        Parser p;
        Gnss g;
        CHECK(send(p, g, kGoodRmc) == Result::Updated);
        CHECK(g.fix);
        CHECK_NEAR(g.lat, 48.1173, 1e-4);
        CHECK_NEAR(g.lon, 11.51667, 1e-4);
        CHECK_NEAR(g.speed_mph, 25.78, 0.02);          // 22.4 knots
        CHECK_NEAR(g.course_deg, 84.4, 1e-3);
        CHECK(g.utc_ms == (12 * 3600 + 35 * 60 + 19) * 1000u);
        CHECK(p.sentences() == 1);
    }

    CASE("fractional seconds in the time field are kept");
    {
        Parser p;
        Gnss g;
        send(p, g, "GPRMC,123519.25,A,4807.038,N,01131.000,E,022.4,084.4,230394,,");
        CHECK(g.utc_ms == (12 * 3600 + 35 * 60 + 19) * 1000u + 250u);
    }

    CASE("a good GGA gives satellites, quality, hdop and altitude");
    {
        Parser p;
        Gnss g;
        CHECK(send(p, g, kGoodGga) == Result::Updated);
        CHECK(g.sats == 8);
        CHECK(g.quality == 1);
        CHECK_NEAR(g.hdop, 0.9, 1e-4);
        CHECK_NEAR(g.alt_m, 545.4, 1e-3);
    }

    CASE("a bad checksum is counted and changes nothing");
    {
        Parser p;
        Gnss g;
        send(p, g, kGoodRmc);
        const Gnss before = g;
        const Result r = feed_all(p, g,
            "$GPRMC,123519,A,0000.000,N,00000.000,E,999.9,999.9,230394,003.1,W*00\r\n");
        CHECK(r == Result::BadChecksum);
        CHECK(p.bad_checksum() == 1);
        CHECK_NEAR(g.lat, before.lat, 1e-12);
        CHECK_NEAR(g.speed_mph, before.speed_mph, 1e-12);
    }

    CASE("a void fix clears the fix flag but keeps the clock and the position");
    {
        Parser p;
        Gnss g;
        send(p, g, kGoodRmc);
        CHECK(g.fix);
        CHECK(send(p, g, "GPRMC,123520,V,,,,,,,230394,,") == Result::Updated);
        CHECK(!g.fix);
        CHECK(g.utc_ms == (12 * 3600 + 35 * 60 + 20) * 1000u);
        // The last known position is left alone rather than zeroed: the map
        // screen would otherwise jump to the Atlantic every time the fix
        // drops under a bridge. `fix` is what says not to trust it.
        CHECK_NEAR(g.lat, 48.1173, 1e-4);
    }

    CASE("GGA quality 0 means no fix whatever else it says");
    {
        Parser p;
        Gnss g;
        send(p, g, kGoodRmc);
        CHECK(g.fix);
        send(p, g, "GPGGA,123519,4807.038,N,01131.000,E,0,00,99.9,0.0,M,,M,,");
        CHECK(!g.fix);
    }

    CASE("an empty course field does not swing the heading to north");
    {
        // A stationary receiver leaves course empty rather than sending 0.
        Parser p;
        Gnss g;
        send(p, g, kGoodRmc);
        CHECK_NEAR(g.course_deg, 84.4, 1e-3);
        send(p, g, "GPRMC,123520,A,4807.038,N,01131.000,E,000.0,,230394,,");
        CHECK_NEAR(g.course_deg, 84.4, 1e-3);
    }

    CASE("other talkers work and other sentences are ignored");
    {
        Parser p;
        Gnss g;
        // GN: a receiver using more than one constellation. The talker id is
        // not fixed and must not be matched on.
        CHECK(send(p, g,
            "GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W")
            == Result::Updated);
        CHECK(g.fix);
        CHECK(send(p, g, "GPGSV,3,1,11,03,03,111,00,04,15,270,00") == Result::Ignored);
        CHECK(send(p, g, "GPVTG,054.7,T,034.4,M,005.5,N,010.2,K") == Result::Ignored);
    }

    CASE("a sentence cut in half does not corrupt the next one");
    {
        Parser p;
        Gnss g;
        for (const char* c = "$GPRMC,123519,A,4807.038,N,0113"; *c; ++c) p.feed(*c, g);
        // The receiver keeps going; the next '$' restarts the parser.
        CHECK(send(p, g, kGoodRmc) == Result::Updated);
        CHECK(g.fix);
        CHECK_NEAR(g.lat, 48.1173, 1e-4);
    }

    CASE("line noise does not run off the end of the buffer");
    {
        Parser p;
        Gnss g;
        p.feed('$', g);
        for (int i = 0; i < 4000; ++i) p.feed('A', g);
        CHECK(p.feed('\n', g) == Result::Malformed);
        CHECK(p.overruns() == 1);
        CHECK(send(p, g, kGoodGga) == Result::Updated);   // and it recovers
        CHECK(g.sats == 8);
    }

    CASE("a truncated sentence with too few fields is refused");
    {
        Parser p;
        Gnss g;
        CHECK(send(p, g, "GPRMC,123519,A") == Result::Malformed);
        CHECK(!g.fix);
    }

    CASE("a sentence with no checksum at all is refused");
    {
        Parser p;
        Gnss g;
        CHECK(feed_all(p, g, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4\r\n")
              == Result::Malformed);
    }

    CASE("bare LF ends a sentence, and so does CRLF");
    {
        Parser p;
        Gnss g;
        char line[160];
        uint8_t sum = 0;
        for (const char* c = kGoodGga; *c; ++c) sum ^= static_cast<uint8_t>(*c);
        std::snprintf(line, sizeof line, "$%s*%02X\n", kGoodGga, sum);
        CHECK(feed_all(p, g, line) == Result::Updated);
        CHECK(g.sats == 8);
    }


    CASE("the RMC date becomes a day of the year");
    {
        Parser p;
        Gnss g;
        send(p, g, kGoodRmc);                    // 23 March 1994
        CHECK(g.day_of_year == 31 + 28 + 23);
        send(p, g, "GPRMC,000000,A,4807.038,N,01131.000,E,0,0,291224,,");   // 2024 is a leap year
        CHECK(g.day_of_year == 364);
        CHECK(g.year == 2024);                   // and as a calendar date
        CHECK(g.month == 12);
        CHECK(g.day == 29);
        send(p, g, "GPRMC,000000,A,4807.038,N,01131.000,E,0,0,010101,,");
        CHECK(g.day_of_year == 1);
    }

    CASE("a missing or impossible date leaves the last good one alone");
    {
        Parser p;
        Gnss g;
        send(p, g, kGoodRmc);
        const uint16_t before = g.day_of_year;
        send(p, g, "GPRMC,123520,A,4807.038,N,01131.000,E,022.4,084.4,,,");
        CHECK(g.day_of_year == before);
        send(p, g, "GPRMC,123521,A,4807.038,N,01131.000,E,022.4,084.4,310224,,");   // no 31 Feb
        CHECK(g.day_of_year == before);
        CHECK(g.month == 3);                     // not half-updated to February
        send(p, g, "GPRMC,123522,A,4807.038,N,01131.000,E,022.4,084.4,231394,,");   // month 13
        CHECK(g.day_of_year == before);
    }

    return check::report("nmea");
}
