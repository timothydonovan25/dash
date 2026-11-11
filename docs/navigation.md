# Turn-by-turn navigation — offline OSM routing from the SD card

**Status (2026-11-03).** Built and tested on the host; **not yet run on the
board.** The map builder, the router, the follower, the state machine, the
`storage` side, the `logic` glue, the NAV screen and three blackbox columns
exist. Host tests: `route`, `route_follow`, `route_navigator`, `route_dest`,
`route_store`, `logic_route` (end to end through `logic_tick`), the NAV
cases in `ui`, and `tools/routegraph/test_restrictions.py`. The firmware
builds. Still to do is §9. The old NAV (an arrow to a waypoint, `geo::guide`, `NavArrow`) is gone.
The waypoints themselves stay, as destinations.

## 1. Rules the rider set

- **Arkansas only**, for now.
- **Speed limit** (2.17): the fastest road a route may use, **50 to 75
  mph in steps of 5, or off**, set on the picker's "Speed limit" row (E up
  5, hold E down 5, round the ends through off) or CONFIG → Nav. A road
  counts at its OSM `maxspeed`; untagged, an interstate (`motorway`) counts
  as 70, a divided highway (`trunk`) as 65 (mostly true in Arkansas), a
  `primary` road as 55, anything else as under any limit
  (`Attr::cap_mph`). A road at the limit is in; over it is out. So 60
  leaves out the divided highways, 65 keeps them, 70 adds untagged
  interstates. **Interstates are on the map** (2.17; one way unless OSM
  says otherwise), for testing in a car with the limit off.
  Nothing is asked: the setting stands for every route until changed. If
  nothing connects under it, the rider is told and may route at any speed
  for that destination (E), or stop (hold E).
- **Dirt** roads (`highway=track`, unpaved surfaces) are routable. The
  picker's last row switches "avoid dirt" on or off.
- **Map updates** come from the card (copy `route/map.rgf`) or from OTA
  (§9).

## 2. Shape

```
Geofabrik .osm.pbf ─► tools/routegraph/extract.py ─► .rgi ─► rgf_build ─► route/map.rgf on SD
                                                                               │
NAV picker ─NavGo─► logic: core::route::Navigator ─Request─► RouteMailbox ─► storage:
                         ▲                                                    serve_route()
                         └── answer + route in RouteData[into] ◄──────────────┘
logic: Follower on RouteData[live] ─► Frame.nav ─► NAV screen; blackbox columns
```

## 3. Building a map (on the PC)

```bash
python3 -m venv ~/osm && ~/osm/bin/pip install osmium
curl -LO https://download.geofabrik.de/north-america/us/arkansas-latest.osm.pbf
~/osm/bin/python tools/routegraph/extract.py arkansas-latest.osm.pbf arkansas.rgi   # ~2 min
cmake -S test -B build-host && cmake --build build-host --target rgf_build
build-host/rgf_build arkansas.rgi map.rgf --check 200                              # ~1 min + check
build-host/rgf_build --rgf map.rgf --route 36.0626,-94.1574 34.7465,-92.2896        # one route, cue by cue
cp map.rgf /path/to/card/route/map.rgf        # or upload it on the Wi-Fi page (Map)
```

**Updating the map on the bike without pulling the card:** CONFIG → SD
card → USB drive (parked), copy `route/` onto the drive, Eject; the dash
restarts on the new map. Measured 2026-11-05: the 122 MB Arkansas map in
146 s (~835 KB/s), verified, and routed on straight after. Over the Wi-Fi
page a state map is ~22 min (68–91 KB/s); use that for a --bbox map or
search.idx alone.

**All of it in one go** (download, extract, map, search index, about 4
minutes for Arkansas):

```bash
tools/routegraph/build_maps.sh north-america/us/arkansas /path/to/out
# then copy /path/to/out/route/ to the card's root
```

**Several states, one map** (2026-11-06): give the regions comma-separated,
`build_maps.sh north-america/us/arkansas,north-america/us/missouri out`, or
several .pbf files to `extract.py`. Neighbouring Geofabrik extracts overlap
at the border (each carries the ways that cross it), so the extractor reads
an OSM id once and counts the rest as `duplicate`; the map's OSM date is
the oldest file's. `test_merge.py` checks a crossroads split in two
overlapping files builds the one-file map and routes across.

For a small map (the bench, a quick upload), `extract.py ... --bbox S W N E`
keeps only the roads in that box: Fayetteville (`35.95 -94.30 36.20
-94.05`) is 22,000 junctions and a few MB.

`extract.py` keeps drivable ways (no motorway, private, no-motor-vehicle,
driveway or parking aisle). It splits them at junctions and splits loops
at their middle, and keeps names (`name`, else `ref`), `maxspeed`,
oneway, link, dirt and roundabout flags, plus fuel, food and parking POIs.

**Turn restrictions** (`no_*`, `only_*`) are applied by node splitting.
Traffic arriving from the `from` road lands on a copy of the junction
whose ways out leave out the banned turn. A restriction across a link
road (via a way, or a chain of them, as a U-turn across a median is
tagged) copies the link's junctions the same way. The router and the dash
need no turn costs. Arkansas: 2,276 junctions and 21 links restricted.
Skipped: 90 incomplete in OSM itself (no `from` or no `to` member), 192
whose `from` road is not on the map (mostly interstate ramps, before
2.17 put interstates on the map), and 13
links with a junction in the middle or a second rule on them.

`rgf_build` (C++, `tools/routegraph/`) numbers the nodes along a Hilbert
curve, routes each road at its posted limit (or a default for its class,
capped at 65 mph), builds a snapping grid of 0.01° cells, and contracts
fourteen **variants**: the speed limit (none, 50, 55, 60, 65, 70, 75)
times dirt allowed or not. A contraction hierarchy cannot leave roads out
at query time, so each is its own graph, ~16 MB of edges on Arkansas: the
whole file is about 270 MB, half of it the no-dirt seven. Format version
2 (2.17): a version 1 map says so on NAV and must be rebuilt.
`--route … --limit 55` routes one as the dash would under that limit.
`--check N` runs N random routes per variant through the dash's own query
code and holds each to plain Dijkstra on the uncontracted roads.

## 4. Measured, Arkansas, OSM 2026-11-02

Before 2.17: four variants, no interstates. The fourteen-variant map with
interstates has not been built yet.

| | |
|---|---|
| Junctions / roads | 560,645 / 703,733 with restrictions applied (interstates excluded: 3,800 km) |
| `maxspeed` tagged | **4 % of road km**: hence the untagged-trunk rule |
| POIs | 594 fuel, 1,283 food, the rest parking (21,000 in all) |
| `map.rgf` | **122 MB** (four variants, ~16 MB of edges each; geometry 25 MB, roads 22 MB) |
| Build | contraction 52 s on the laptop |
| Self-check | 800 routes, **0 mismatches** with Dijkstra |
| Search | ~250 nodes settled per route, 416 at most |
| Card reads, cold 4 MB cache | ~600 × 4 kB blocks per route, 1,200 at most; most are geometry |
| Fayetteville → Little Rock | 181 mi, 3 h 29 min, no interstate; 906 block reads cold |
| **On the bike** (2.5, `route bench`) | **4.5 s cold, 2.8 s warm**: ~5 ms a 4 kB block with the file held open and FATFS fast seek. It was 28.3 s (31 ms a block) with an `fopen` per read |

Not measured yet: the time per block read on the board (§9).

## 5. Code

| Piece | Where | Test |
|---|---|---|
| File format | `core/route/rgf.hpp` | `route` |
| Block cache, reader | `core/route/graph.hpp`, `src/route_graph.cpp` | `route` |
| Snap, CH query, unpack, cues | `core/route/route.hpp`, `src/route_query.cpp` | `route` |
| Following, off-route, arrival | `core/route/follow.hpp` | `route_follow` |
| State machine (speed limit, reroute) | `core/route/navigator.hpp` | `route_navigator` |
| Favourites file, nearest POI | `core/route/dest.hpp` | `route_dest` |
| Mailbox, card source, serve | `services/route_store.hpp` | `route_store` |
| `logic` glue | `logic_tick.cpp` (`pump_route`, `publish_nav`, `dead_reckon`) | none yet (§9) |
| Screen | `NavScreen` in `screens_cruise.cpp`, `TurnArrow` | `ui` |
| Builder | `tools/routegraph/` | `route` (builds its town with it) |

Points to know:
- **Cues.** A cue is a junction where the route turns ≥ 30° (measured over
  20 m either side) or the road's name changes. "East North St" becoming
  "West North St" doesn't count. Kinds run straight, slight, turn, sharp
  and U-turn, then arrive.
- **Roundabouts** are one cue at the way in, drawn as a ring with the exit
  at its real angle and named for the road out. There is no "exit N": the
  graph cannot tell an exit from a point where the ring's way is merely
  split, and a wrong number is worse than none.
- **Off route.** More than 50 m off the line for 5 s triggers a reroute
  from the bike's position. Reroutes are at most one per 10 s, and a failed
  one keeps the old route. Arrival is within 30 m.
- **On a road over the speed limit** (a wrong turn onto the highway, or
  starting from one): a route can only start on a road the rider is on,
  so when the nearest road is one over the limit within 50 m, the query
  under the limit answers "no start" (`route_query.cpp`; a frontage road
  nearby does not count). The navigator then routes once with no limit,
  and puts it back as soon as the bike is on a road of that route under
  it, or leaves it by an exit of its own (2.6, `Navigator::detour_`).
  Riding under a highway overpass can briefly look like being on it: a
  detour, then a reroute 5 s later.
- **The posted limit** shown is held: a new value shows once the bike has
  been on it 2 s (`FollowConfig::limit_hold_ms`, 2.17). At a junction the
  nearest segment can swap between two roads, and an untagged stub between
  two tagged roads reads 0, so the raw value flickered.
- **Between GPS fixes** (1 Hz), `logic` carries the position forward
  along the course at the fused speed, up to 2 s.
- **Ownership.** `logic` owns `RouteData[live]`. `storage` writes the
  other buffer, and only while it holds the mailbox's turn.

## 6. Memory and tasks

- PSRAM: `g_route` (two route buffers, 1.6 MB) and `g_route_engine`
  (4 MB block cache plus the search, 4.6 MB).
- The `storage` task (core 1) serves one job per 500 ms pass. Its stack
  went from 4 to 5 kB.
- `render`'s stack grew by `sizeof(Nav)`, because the Frame grew.
- Nothing new runs on core 0 except the follower, which works on points
  already in RAM.

## 7. Destinations

The picker lists, in order:
0. **Where to?**: an address, town or place spelled with the pod (§7.1).
1. The **waypoints** that are set (START, TRUCK, CAMP, FUEL, MARK). Hold E
   on one drops it here, as before.
2. **`route/destinations.csv`** on the card: `name,lat,lon` per line,
   `#` for comments, up to 3 shown, names cut to 15 characters.
3. **Recent**: the last places gone to through Where to? or the phone
   (up to 3 shown, 5 kept in `route/recent.csv`, newest first; within
   50 m is the same place). Written to the card, not flash, so going
   somewhere while riding costs core 0 nothing.
4. The **2 nearest fuel** and **2 nearest food** places from the map,
   refreshed every 60 s.
5. A last row, **Dirt roads: allowed / avoid**: the setting CONFIG → Nav
   → Avoid dirt (key 0x1101), saved like any other.

6. **From the phone**: Feed tag `G <lat> <lon> [name]` (radio-and-updates
   §11.1). It goes first in the list and is routed to at once. The dash
   side is built and tested (`parse_go`, `PhoneDest`, `logic_route`); the
   app sends it from Settings → "Send a destination to the dash"
   (`~/phone`, `NavProto.kt`). Works phone to bike (2026-11-05). The
   Sonim has no Google Play services, so no Android Geocoder: the app
   looks addresses up with OpenStreetMap's Nominatim instead
   (`NavLookup.kt`; it needs a signal). Typing "lat, lon" needs none.

### 7.1 Where to? (address search on the dash)

Like an old Garmin, offline from `route/search.idx`, spelled with the pod.
- **What it finds:** every named street (with its town), towns and
  villages, and named places (fuel, food, shops, parks...). It finds them
  by the start of any real word in the name: "COLLEGE" finds "N College
  Ave". Compass points and street types (N, AVE, ST...) don't start a key.
- **Names as signed:** "North College Avenue" is stored as "N College
  Ave", which is what a rider types and what fits on the screen.
- **The keyboard:** a 13 × 3 grid of DEL, LIST (or GO), A–Z, 0–9 and
  space. Only keys that lead to a match can be picked; the rest are
  dimmed, as on a Garmin.
  - The screen moves the cursor itself (C/D, over the keys on offer).
    It starts on the first letter, and after a key it stays on that key
    if it is still on offer, else moves to the nearest one that is, so
    typing in the digits stays in the digits.
  - DEL and LIST (or GO) are the first two keys of the grid.
  - E presses a key; hold E closes the search.
- **Results:** nearest first. A town whose whole name was typed comes
  first ("LITTLE ROCK" is the city, not Little Rock Rd 2 mi away).
  Results are listed from 3 letters, or sooner once fewer than 300 match,
  so a one-letter search doesn't read half the index.
- **Number first, as on a Garmin:** "13511 RYAN" (a house number, a
  space, then the street).
  - While the number is being typed, digits and space stay on offer, and
    from 3 digits the street's first letters too: a letter puts the space
    in by itself (1-3-5-1-1-R is "13511 R"), so the space key is never
    needed.
  - The results are that house on each street found, best fit first: a
    number OSM has, then one between two it has, then streets whose
    numbers end before it, then streets with none; nearest first within
    each.
  - Picking one goes straight there. "13511 RYAN" lists 13511 Ryan Rd,
    Pea Ridge (exact) before the Ryan Rd in Bentonville that has no
    numbers.
- **Or number last:** pick a street that has numbers and the keys become
  digits, then GO.
  - A number OSM has is exact.
  - Otherwise the house is placed between the nearest numbers either side
    on that street, in proportion.
  - GO with no number goes to the street itself.
- **Where it runs:** `core::route::SearchIndex` (`search.hpp`) over its own
  1 MB block cache, served by `storage` as two mailbox jobs, Search and
  House. `logic` holds what was typed (commands `NavSearch`,
  `NavSearchKey`, `NavSearchPick`, `NavSearchGo`), and `Frame::nav`
  carries the keyboard and results to the screen.
- **Arkansas:** 148,019 entries, 288,602 house numbers, 264,357 keys,
  18.6 MB. A search reads 20–40 blocks; "N COLLEGE", then 1200, gives
  1200 N College Ave in Fayetteville, exact.

**Built by** `extract.py`, which writes a `.sxi` of raw entries next to its
`.rgi`:
- **Addresses:** `addr:housenumber` + `addr:street` on nodes and
  buildings.
- **Towns:** `place=city/town/village/hamlet/suburb`.
- **Places:** named amenity, shop, tourism and leisure.
- **Streets:** the routable ways' names, grouped by name and town. A
  street's town is its addresses' `addr:city`, else the biggest place
  whose reach covers it: city 10 km, town 5 km, village 2 km.

Then `rgf_build --search x.sxi search.idx` writes the sorted index, and
`rgf_build --idx search.idx --find TEXT LAT,LON [NUMBER]` searches it the
way the dash does.

### 7.2 Pace notes (2.8)

Rally-style notes for the bends between the turn cues, worked out from the
route's own shape (`core/route/pace.hpp`, `test_route_pace`):

- The polyline is resampled every 5 m along `dist_dm`; the heading change
  across 20 m either side of a sample is its curvature. A bend starts
  above 1/250 m⁻¹ and ends below 1/400 m⁻¹ or when it turns the other way.
  Under 15° of turn is not a note; a bend within 30 m of a turn cue is the
  turn itself, which the arrow already gives.
- **Grade** from the tightest radius: 1 under 20 m, 2 under 40, 3 under
  70, 4 under 110, 5 under 170, 6 above. **hairpin**: 150° or more at
  grade 1–2. **long**: 150 m and 60° or more. **tightens / opens**: the
  last third's mean curvature 1.4× the first's, or the reverse (bends of
  40–640 m). **into**: starts within 30 m of the bend before.
- `logic` (`publish_nav`) works out the bends in the next 800 m from the
  follower's `along_dm`, once a second, into `Nav::pace` (three at most);
  nothing is stored for the whole route, so a 400 km route costs the
  same.
- The guidance status line reads them as a co-driver would, when it has
  nothing more urgent (off route, a road over the limit): the distance to the
  first, each note, then the gap to the next unless it runs into it:
  `300 R4 tightens 150 L3 into R5`.

Limits: OSM geometry only. A road drawn with few points reads sharper at
its corners than it is, and there is no elevation, so no crests or dips.
The thresholds are first guesses. The Pig Trail (AR 23, Ozark to AR 16)
gives 26 notes in 17.5 km, grades 2–5, a hairpin past the Mulberry: the
road has more bends than that, and a ride should set the numbers.

## 8. Screen and log

NAV has three views (picker, Where to?, guidance), each tiling the body
exactly (no overlaps, no gaps), so switching between them repaints every
pixel. Guidance shows the arrival time on the clock beside the miles and
minutes left, as a Garmin does.
- **Picker:** a header line ("MAP yyyy-mm-dd", "NO MAP: …" or "NO ROUTE:
  why"), then the list.
- **Guidance:** a turn arrow, the distance to the turn, the road after it,
  the posted limit (blank when the map has none), then distance and
  minutes left. A status line shows Routing, Off route / rerouting, "Road
  over 55 mph" (a detour), "No route under 55 mph" (the rider's limit), or
  Arrived. CRUISE →
  SPEED shows the same posted limit under the heart rate (2.17).
- **Route view** (2.7), in place of the arrow and the numbers: short E
  while guiding, and by itself when no route fits the limit. `MapView` in
  route-only mode over the same `RouteTrace` as CRUISE → MAP: fitted to
  the route alone, its stretches over the speed limit amber (`trace_route`
  puts 65 in a thinned point's `speed_mph` when any segment since the last
  point is over it), the destination ringed, the bike white, its 2-4
  longest-ridden roads named (`longest_roads`, 2.18). North up; no other
  roads.

Blackbox columns, appended at the end: `NavTurnFt`, `NavDestMi`, `LimitMPH`
(0 when not routing or unknown).

## 9. Not done yet

Done since the first cut:
- the `logic_route` test;
- map upload on the Wi-Fi page (`PUT /route/map.rgf`: written to
  `route/map.tmp`, then over the old map; the first chunk must be an .rgf);
- turn restrictions via a node, a way, or a chain of ways;
- roundabout cues;
- the route on CRUISE → MAP (`services::RouteTrace`: `logic` thins the
  route to 2,048 points in the trip trace's frame, and `MapView` draws it
  dim under the trace and fits both), and NAV's route view (§8, 2.7; seen
  in the simulator, not yet on the board);
- Avoid dirt as a saved setting;
- the phone's `G` destination, dash side and app side: `~/phone`
  Settings → "Send a destination to the dash" (`NavProto.kt`, the
  Geocoder, or "lat, lon" typed in);
- a map for the simulator (`tools/routegraph/sim_map.py`, a made-up town
  where the sim bike rides);
- **Where to?** (§7.1): address, town and place search on the dash, with
  house numbers, in the host tests, the `logic_route` test and the
  simulator (its town has a search index too), and on the bike: 2.5
  finds 13511 Ryan Rd, Pea Ridge from 1-3-5-1-1-R-Y-A-N and routes there.

Still open:

1. **On the board.** Nothing here has run on the P4. The bench:
   1. Build a map (§3) and copy it, with a `route/destinations.csv`
      holding a far destination, to the card. (Done 2026-11-03: the
      3D-DRIVE card has `route/map.rgf`, Arkansas OSM 2026-11-02, and a
      "Little Rock" favourite. The card was in the laptop then, not the
      bike.)
   2. Flash, and on 12 V wait for a fix.
   3. Go to CRUISE → NAV (`btn B` from `tools/pod_keys.py`), pick the far
      favourite, press E.
   4. `storage` logs each query: `route: ok in N ms, S settled, B block
      reads, P points`.

   For Where to?, also copy `route/search.idx` (`build_maps.sh` makes
   both), then spell something with `btn C`/`btn D`/`btn E` from
   `tools/pod_keys.py` (the pod on this board has not run yet). Each
   Search and House job is a `storage` job like a route.

   (Measured 2026-11-05: 4.5 s cold for 181 mi once the driver held the
   file open; see §4.) Done on the bench since: routing from the card
   (2.5), Where to? for one address (13511 Ryan Rd), the phone's `G` for
   one address, and the map over USB (2.6: 146 s for 122 MB). A hotspot
   upload of the 122 MB map timed out at 1,200 s (68 kB/s); the 18.6 MB
   index took 205 s. Where to?, scripted over USB (console `screen` to read
   the keys, `btn` to press them; 2026-11-06, 2.6):
   - "LITTLE ROCK": the town first of 204 matches; 209 mi, 246 min,
     routed in 5.0 s; the 65 mph prompt came up, and E (avoid) rerouted
     in 4.3 s.
   - "DICKSON ST": 18 matches, nearest first; picking one asks for the
     house number (or GO for the street).
   - "13511 RYAN": the number first, then the street; "13511 Ryan Rd"
     first, routed in 0.5 s.
   - "WALMART": 173 matches, the nearest first, routed in 0.3 s.

   Each keypress's search took 10–380 ms; the empty query 0.4–0.8 s.
   A route used to raise LOG STOPPED, for two reasons, both fixed in 2.6:
   `storage` (then priority 3) retook the card lock for each of a long
   route's ~1,100 reads, so the logger waited out its 3 s lock wait
   (slowest write 3,000 ms; now 86–104 ms with `storage` at priority 1);
   and the storage loop's 2 Hz "card fine" report stopped for the
   route's 5 s, so SD health went stale, then Fault. The route's own good
   reads now count as that report (`ReadsKeepAlive`). Routing is no
   slower (4,958–4,977 ms either way). The first scripted try also reset
   the dash into the ROM SHA boot loop (bringup §2); not seen again in
   about 20 routes and 60 searches since.

   The map rebuilt with the per-pair restrictions (§9 item 3; OSM
   2026-11-03, built 2026-11-06) is on the card but not yet routed on the
   dash. On the host, through `core::route` itself (`query`,
   `SearchIndex::find`) from Dickson St, Fayetteville, it routes Walmart
   (1.6 km) and Little Rock (291.6 km, 50 cues), both with and without
   65 mph roads, the same as the old map; 363 nodes settled at most.

   Off-route rerouting runs end to end in the host tests
   (`test_logic_route`: off the slow route onto the highway, rerouted
   along it). Writing that test found that a rider who strayed onto a
   65 mph road while avoiding them was rerouted from a nearby slow road
   they were not on, or (none within 500 m) never rerouted; fixed in 2.6
   (§5, "On a 65 mph road while avoiding them").

   On the road: everything in §7 that needs a fix and a moving bike
   (reroute, cue timing, the 65 mph prompt while riding).
2. **No U-turn penalty.** Routing is node-based with no turn costs, so
   where a restriction bans a turn, the route may carry on to the next
   junction, U-turn and come back. That is legal, and often what people
   do, but a turn-cost (edge-based) graph would prefer going round the
   block. Not worth doubling the graph until a ride shows it matters.
3. **Restrictions the extractor skips** are counted in its summary
   (`restriction-*`). Arkansas, OSM 2026-11-03 (checked 2026-11-06):
   2,279 junctions and 21 via-way chains carry restrictions. Skipped:
   - 188 `from-off-map` and 28 `off-map`: a road or junction left off the
     map (mostly interstate ramps). A route can never meet them: no loss.
   - 84 `unsupported`: relations missing their `from`, `to` or via member
     (cut at the state line or mistagged). Nothing to apply.
   - 19 `no-from`: the `from` road is on the map but never arrives at the
     junction (one-way leaving it, or mistagged).
   - 13 via-way skips: split by a junction (1), overlapping another (8),
     not joined end to end (4).

   So about 32 real restrictions are not applied statewide. Relations
   with several `from` or `to` roads are split into one rule per pair
   since 2.6 (`test_restrictions.py`); that added 3 junctions.

## 10. Building and using a map in the simulator

```bash
python3 tools/routegraph/sim_map.py build-host/rgf_build sim_card
```

This writes `sim_card/route/map.rgf` (a 9 × 9 street grid around the sim
bike, an untagged divided highway M-59 along the south edge so the 65 mph
question comes up, a one-way Main St and a gravel road) and two
favourites. CRUISE → NAV in the sim then routes for real, through the
firmware's own `logic_tick` and `serve_route`.
