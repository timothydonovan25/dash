#include <cstring>
#include "check.hpp"
#include "core/route/dest.hpp"

using namespace core::route;

int main() {
    CASE("a favourites line: name, lat, lon");
    {
        Dest d;
        const char* l = "Home,36.0626,-94.1574\r\n";
        CHECK(parse_dest_line(l, std::strlen(l), d));
        CHECK(std::string(d.name) == "Home");
        CHECK(d.p.lat_e7 == 360626000);
        CHECK(d.p.lon_e7 == -941574000);
        CHECK(d.kind == 0);
    }

    CASE("spaces are trimmed and a long name is cut to fit");
    {
        Dest d;
        const char* l = "Devil's Den State Park Campground , 35.78 , -94.25";
        CHECK(parse_dest_line(l, std::strlen(l), d));
        CHECK(std::strlen(d.name) == kDestNameLen - 1);
        CHECK(std::strncmp(d.name, "Devil's Den Sta", 15) == 0);
        CHECK(d.p.lat_e7 == 357800000);
    }

    CASE("comments, blanks and nonsense are skipped");
    {
        Dest d;
        const char* bad[] = {"", "# name,lat,lon", "Home,36.0", "Home,abc,-94", ",36,-94",
                             "Home,96,-94", "Home,36,-194", "Home,36,-94x"};
        for (const char* l : bad) CHECK(!parse_dest_line(l, std::strlen(l), d));
    }

    CASE("a whole file, capped at the room given");
    {
        const char* f = "# favourites\nHome,36.06,-94.15\n\nWork,36.1,-94.2\nbroken\nCabin,35.9,-93.9\n";
        Dest out[2];
        CHECK(parse_dests(f, std::strlen(f), out, 2) == 2);
        CHECK(std::string(out[1].name) == "Work");
        Dest all[8];
        CHECK(parse_dests(f, std::strlen(f), all, 8) == 3);
        CHECK(std::string(all[2].name) == "Cabin");
    }

    CASE("the phone's G line: where, and what to call it");
    {
        Dest d;
        const char* l = "G 36.062600 -94.157400 Dickson St Pub\n";
        CHECK(parse_go(l, std::strlen(l), d));
        CHECK(d.p.lat_e7 == 360626000 && d.p.lon_e7 == -941574000);
        CHECK(std::string(d.name) == "Dickson St Pub");
        const char* bare = "G 35.5 -93.25";
        CHECK(parse_go(bare, std::strlen(bare), d));
        CHECK(std::string(d.name) == "PHONE");
        const char* longname = "G 35.5 -93.25 1600 Pennsylvania Avenue Northwest";
        CHECK(parse_go(longname, std::strlen(longname), d));
        CHECK(std::strlen(d.name) == kDestNameLen - 1);
        const char* bad[] = {"G", "G ", "G 35.5", "G 95 -93", "G 35 -193 x", "T 35 -93", "G35 -93",
                             "G abc -93 x"};
        for (const char* b : bad) CHECK(!parse_go(b, std::strlen(b), d));
    }

    CASE("a recent place written and read back the same");
    {
        Dest d;
        std::strcpy(d.name, "Pub, the");
        d.p = Point{360626000, -941574000};
        char line[64];
        const size_t n = format_dest_line(d, line, sizeof line);
        CHECK(n > 0 && line[n - 1] == '\n');
        CHECK(std::string(line) == "Pub  the,36.0626000,-94.1574000\n");
        Dest back;
        CHECK(parse_dest_line(line, n, back));
        CHECK(back.p.lat_e7 == d.p.lat_e7 && back.p.lon_e7 == d.p.lon_e7);
        CHECK(format_dest_line(d, line, 10) == 0);      // too small: nothing
        d.p = Point{-5, 5};                              // small and negative
        format_dest_line(d, line, sizeof line);
        CHECK(std::string(line) == "Pub  the,-0.0000005,0.0000005\n");
    }

    CASE("recent: newest first, the same place once, five at most");
    {
        Dest old[5];
        for (int i = 0; i < 5; ++i) {
            std::snprintf(old[i].name, sizeof old[i].name, "Place %d", i);
            old[i].p = Point{360000000 + i * 100000, -941000000};   // ~1.1 km apart
        }
        Dest fresh;
        std::strcpy(fresh.name, "Again");
        fresh.p = Point{360000000 + 2 * 100000 + 10, -941000000};   // 1 m from Place 2
        Dest out[5];
        const size_t n = push_recent(fresh, old, 5, out, 5);
        CHECK(n == 5);
        CHECK(std::string(out[0].name) == "Again" && out[0].kind == kDestRecent);
        CHECK(std::string(out[1].name) == "Place 0");
        CHECK(std::string(out[2].name) == "Place 1");
        CHECK(std::string(out[3].name) == "Place 3");   // Place 2 is the same spot as Again
        CHECK(std::string(out[4].name) == "Place 4");
        CHECK(push_recent(fresh, old, 0, out, 5) == 1);
    }

    return check::report("route_dest");
}
