// Saved tracks: which one a fix belongs to, where a new line goes, what a
// lap may change, and the file.
#include "check.hpp"
#include "core/track/track.hpp"
#include <cmath>
#include <cstring>
#include <vector>

using namespace core::track;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLat0 = 40.0;
constexpr double kLon0 = -105.0;

core::geo::LatLon at_ft(double x, double y) {
    return {kLat0 + y / 364000.0,
            kLon0 + x / (364000.0 * std::cos(kLat0 * kPi / 180.0))};
}

Record saved(double x_ft, double y_ft, uint32_t day = 100) {
    return fresh(Line{at_ft(x_ft, y_ft), 90.0}, day);
}

}  // namespace

int main() {
    CASE("a fix brings back the nearest saved track within reach, and only that");
    {
        Library lib;
        lib.rec[3] = saved(0, 0);
        lib.rec[7] = saved(800, 0);
        CHECK(nearest(lib, at_ft(100, 50)) == 3);
        CHECK(nearest(lib, at_ft(700, 0)) == 7);
        // A pee-wee loop beside the main track: the nearer line wins.
        CHECK(nearest(lib, at_ft(450, 0)) == 7);
        CHECK(nearest(lib, at_ft(0, kRecallFt + 50)) == -1);
        CHECK(nearest(Library{}, at_ft(0, 0)) == -1);
        // An empty slot is not a track, wherever its zeroed line would be.
        Library empty;
        empty.rec[0].finish.at = at_ft(0, 0);
        CHECK(nearest(empty, at_ft(0, 0)) == -1);
    }

    CASE("a line set near a saved one moves it; further away it is a new track");
    {
        Library lib;
        lib.rec[0] = saved(0, 0);
        lib.rec[1] = saved(5000, 0);
        CHECK(slot_for_line(lib, at_ft(kSameTrackFt - 20, 0)) == 0);
        CHECK(slot_for_line(lib, at_ft(5000, 40)) == 1);
        CHECK(slot_for_line(lib, at_ft(kSameTrackFt + 200, 0)) == 2);
    }

    CASE("a full library gives up the track ridden longest ago");
    {
        Library lib;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            lib.rec[i] = saved(10000.0 * static_cast<double>(i), 0, 200 + static_cast<uint32_t>(i));
        }
        lib.rec[9].last_day = 150;
        CHECK(slot_for_line(lib, at_ft(-90000, 0)) == 9);
        // ... but a line on a saved track is still that track.
        CHECK(slot_for_line(lib, at_ft(30000, 10)) == 3);
    }

    CASE("only a clean lap quicker than the record becomes it");
    {
        Record r = saved(0, 0);
        CHECK(r.best_ms == 0);
        CHECK(!offer_lap(r, 60000, false));        // gapped: may be two laps
        CHECK(r.best_ms == 0);
        CHECK(offer_lap(r, 60000, true));
        CHECK(!offer_lap(r, 60000, true));         // a tie is not a record
        CHECK(!offer_lap(r, 61000, true));
        CHECK(offer_lap(r, 59500, true));
        CHECK(r.best_ms == 59500);
        CHECK(!offer_lap(r, 0, true));
    }

    CASE("the session's sector bests improve the record a sector at a time");
    {
        Record r = saved(0, 0);
        CHECK(r.ideal_ms() == 0);
        const uint32_t first[3] = {20000, 0, 0};
        CHECK(offer_sectors(r, first));
        CHECK(r.ideal_ms() == 0);                  // two still missing
        const uint32_t more[3] = {21000, 19000, 18000};
        CHECK(offer_sectors(r, more));
        CHECK(r.sector_best_ms[0] == 20000);       // the slower one did not win
        CHECK(r.ideal_ms() == 57000);
        CHECK(!offer_sectors(r, more));
    }

    CASE("points move between the line's frame and this power-on's");
    {
        core::delta::Point p[2] = {{1.0f, 2.0f, 0}, {-3.0f, 4.0f, 100}};
        shift(p, 2, 10.0f, -5.0f);
        CHECK_NEAR(p[0].x_ft, 11.0, 1e-6);
        CHECK_NEAR(p[1].y_ft, -1.0, 1e-6);
        shift(p, 2, -10.0f, 5.0f);
        CHECK_NEAR(p[1].x_ft, -3.0, 1e-6);
        CHECK(p[1].elapsed_ms == 100);
    }

    CASE("crc32 is the standard one");
    {
        const char* s = "123456789";
        CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
    }

    CASE("a track and its reference survive the file");
    {
        Record r = saved(12.5, -40.25, 321);
        r.splits = true;
        r.split[0] = Line{at_ft(400, 400), 12.5};
        r.split[1] = Line{at_ft(-400, 400), 271.0};
        r.best_ms = 61234;
        r.sector_best_ms[0] = 20001;
        r.sector_best_ms[1] = 20002;
        r.sector_best_ms[2] = 20003;
        r.ref_ms = 61500;
        r.sessions = 7;
        std::vector<core::delta::Point> pts(600);
        for (size_t i = 0; i < pts.size(); ++i) {
            pts[i] = {static_cast<float>(i) * 1.5f, -static_cast<float>(i) * 0.25f,
                      static_cast<uint32_t>(i * 100)};
        }
        static uint8_t buf[kMaxFileBytes];
        const size_t len = encode(r, pts.data(), pts.size(), buf, sizeof buf);
        CHECK(len == kHeaderBytes + pts.size() * kPointBytes + 4);

        Record back;
        static core::delta::Point got[core::delta::kMaxPoints];
        size_t n = 0;
        CHECK(decode(buf, len, back, got, core::delta::kMaxPoints, n));
        CHECK(n == pts.size());
        CHECK(back.used && back.splits);
        CHECK(back.finish.at.lat == r.finish.at.lat);
        CHECK(back.finish.at.lon == r.finish.at.lon);
        CHECK(back.finish.heading_deg == 90.0);
        CHECK(back.split[1].heading_deg == 271.0);
        CHECK(back.split[0].at.lat == r.split[0].at.lat);
        CHECK(back.best_ms == 61234);
        CHECK(back.sector_best_ms[2] == 20003);
        CHECK(back.ref_ms == 61500);
        CHECK(back.last_day == 321);
        CHECK(back.sessions == 7);
        CHECK(got[599].x_ft == pts[599].x_ft);
        CHECK(got[599].elapsed_ms == 59900);

        // The header alone, for the list, says how many points follow.
        Record head;
        size_t want = 0;
        CHECK(decode_header(buf, kHeaderBytes, head, want));
        CHECK(want == 600);
        CHECK(head.best_ms == 61234);
    }

    CASE("a damaged file is not believed, and damaged points cost only the reference");
    {
        Record r = saved(0, 0);
        r.best_ms = 50000;
        r.ref_ms = 50000;
        core::delta::Point pts[3] = {{0, 0, 0}, {1, 1, 100}, {2, 2, 200}};
        uint8_t buf[kHeaderBytes + 3 * kPointBytes + 4];
        const size_t len = encode(r, pts, 3, buf, sizeof buf);
        CHECK(len == sizeof buf);

        Record back;
        core::delta::Point got[3];
        size_t n = 99;

        // Cut short by a power cut, mid-points: the track is good, the
        // reference is not.
        CHECK(decode(buf, len - 5, back, got, 3, n));
        CHECK(n == 0);
        CHECK(back.best_ms == 50000);

        // One flipped bit in the points.
        buf[kHeaderBytes + 4] ^= 0x10;
        CHECK(decode(buf, len, back, got, 3, n));
        CHECK(n == 0);
        buf[kHeaderBytes + 4] ^= 0x10;
        CHECK(decode(buf, len, back, got, 3, n));
        CHECK(n == 3);

        // One flipped bit in the header: not a track at all.
        buf[40] ^= 0x01;
        CHECK(!decode(buf, len, back, got, 3, n));
        buf[40] ^= 0x01;

        // Not ours, or too short to be.
        uint8_t other[kHeaderBytes] = {};
        size_t w = 0;
        CHECK(!decode_header(other, sizeof other, back, w));
        CHECK(!decode_header(buf, kHeaderBytes - 1, back, w));
        // No room: nothing written.
        CHECK(encode(r, pts, 3, buf, sizeof buf - 1) == 0);
        CHECK(encode(r, pts, core::delta::kMaxPoints + 1, buf, sizeof buf) == 0);
    }

    CASE("a track with no reference yet is a file with no points");
    {
        Record r = saved(0, 0);
        r.ref_ms = 777;           // no points to go with it, so not written
        uint8_t buf[kHeaderBytes + 4];
        const size_t len = encode(r, nullptr, 0, buf, sizeof buf);
        CHECK(len == sizeof buf);
        Record back;
        size_t n = 1;
        CHECK(decode(buf, len, back, nullptr, 0, n));
        CHECK(n == 0);
        CHECK(back.ref_ms == 0);
    }

    CASE("each slot has its own file, and a temporary beside it");
    {
        char name[32];
        CHECK(file_name(name, sizeof name, 0));
        CHECK(std::strcmp(name, "tracks/t00.trk") == 0);
        CHECK(file_name(name, sizeof name, 15, true));
        CHECK(std::strcmp(name, "tracks/t15.tmp") == 0);
        CHECK(!file_name(name, sizeof name, kMaxTracks));
        CHECK(!file_name(name, 8, 1));
    }

    return check::report("track");
}
