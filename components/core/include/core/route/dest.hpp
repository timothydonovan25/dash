// Where the rider can ask to go without typing (docs/navigation.md §7):
// the favourites file on the card, and the map's nearest fuel and food.
// The named waypoints are the third source; `logic` adds those itself.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/route/graph.hpp"

namespace core::route {

inline constexpr size_t kDestNameLen = 16;   // with the NUL; the list is narrow

struct Dest {
    char    name[kDestNameLen] = {};
    Point   p{};
    uint8_t kind = 0;            // 0 favourite, kDestRecent, else PoiKind
};
inline constexpr uint8_t kDestRecent = 9;

// One line of route/recent.csv (the same form as destinations.csv), with
// its newline. Returns its length, 0 if it did not fit.
size_t format_dest_line(const Dest& d, char* out, size_t cap);

// `fresh` first, then `old` without it (the same place twice is one), at
// most `max`. The places a Garmin keeps under "Recent".
size_t push_recent(const Dest& fresh, const Dest* old, size_t n_old, Dest* out, size_t max);

// One line of route/destinations.csv: `name,lat,lon`, degrees. Blank lines,
// '#' comments and anything unreadable are false. The name is cut to fit.
bool parse_dest_line(const char* line, size_t len, Dest& out);

// The phone's `G` Feed line (radio-and-updates §11.1): `G <lat> <lon> [name]`,
// decimal degrees, the name the rest of the line (cut to fit; "PHONE" when
// absent). An address typed on the phone, geocoded there; the dash routes.
bool parse_go(const char* line, size_t len, Dest& out);

// Up to `max` favourites from a whole file in `text`. Returns the count.
size_t parse_dests(const char* text, size_t len, Dest* out, size_t max);

// The `max` POIs of `kind` nearest `here`, nearest first, named by the map
// (or by their kind, unnamed). Returns the count; 0 on a read error too.
size_t nearest_pois(Graph& g, Point here, uint8_t kind, Dest* out, size_t max);

}  // namespace core::route
