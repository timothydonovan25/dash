#include "check.hpp"
#include "core/waypoint/waypoint.hpp"

using namespace core::waypoint;
using core::state::Waypoint;

int main() {
    CASE("the names are fixed and START comes first");
    CHECK(std::string(name(Start)) == "START");
    CHECK(std::string(name(Mark)) == "MARK");
    CHECK(std::string(name(99)) == "?");

    CASE("START moves for a new ride and stays for a lunch stop");
    {
        const uint32_t today = gps_day(2026, 286);
        Waypoint s;
        CHECK(start_is_stale(s, today, 0));                     // never set
        s.set = true;
        s.day = today;
        s.utc_ms = 15u * 3600000u;                               // 15:00
        CHECK(!start_is_stale(s, today, 16u * 3600000u));       // key back on at 16:00
        CHECK(start_is_stale(s, today, 22u * 3600000u));        // 7 h later: a new ride
        CHECK(start_is_stale(s, gps_day(2026, 287), 1u * 3600000u));   // next day
        CHECK(!start_is_stale(s, 0, 0));                        // no date: keep it
    }

    CASE("a waypoint survives NVS as three doubles");
    {
        Waypoint w;
        w.set = true;
        w.lat = 42.123456789;
        w.lon = -83.987654321;
        w.day = gps_day(2026, 286);
        w.utc_ms = 51234567;
        Waypoint back;
        decode(w.lat, w.lon, encode_when(w), back);
        CHECK(back.set);
        CHECK(back.lat == w.lat && back.lon == w.lon);
        CHECK(back.day == w.day);
        CHECK(back.utc_ms == w.utc_ms);
        decode(1.0, 2.0, encode_when(Waypoint{}), back);        // cleared
        CHECK(!back.set);
    }

    CASE("only a good fix is remembered");
    {
        core::state::Gnss g;
        CHECK(!good_fix(g));
        g.fix = true;
        g.hdop = 1.1f;
        CHECK(good_fix(g));
        g.hdop = 6.0f;
        CHECK(!good_fix(g));
    }

    return check::report("waypoint");
}
