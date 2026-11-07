#!/usr/bin/env python3
"""OSM extract -> raw road graph (.rgi), the first half of a map build.

    extract.py arkansas-latest.osm.pbf arkansas.rgi
    extract.py arkansas-latest.osm.pbf missouri-latest.osm.pbf ar-mo.rgi

Several extracts make one map. Neighbouring Geofabrik extracts overlap at
the border (each carries the other's ways that cross it), so anything
already read, by OSM id, is skipped.

Second half: `rgf_build arkansas.rgi arkansas.rgf` (contraction, layout).
docs/navigation.md §3 has the whole pipeline.

Keeps roads a street-legal 250 may use: no motorway (interstates and
freeways are dropped here, so no route can ever use one), no private or
no-motor-vehicle ways, no driveways or parking aisles. Dirt tracks are
kept and flagged. Ways are split at junctions; the points between two
junctions become the edge's geometry.

The .rgi layout (little-endian) is read by tools/routegraph/rgi.hpp; the
two must agree, and the round-trip test in test/test_routegraph.cpp checks
a file this script wrote (test/data/route_town.rgi).

Needs pyosmium (`pip install osmium`).
"""
import argparse
import datetime
import math
import os
import re
import struct
import sys
from collections import Counter

try:
    import osmium
    _Handler = osmium.SimpleHandler
except ImportError:          # sim_map.py and the .rgi writer need no OSM
    osmium = None
    _Handler = object

# Road classes: the numbers are core::route::RoadClass (core/route/rgf.hpp).
CLASS = {
    "trunk": 1, "primary": 2, "secondary": 3, "tertiary": 4,
    "unclassified": 5, "residential": 6, "living_street": 7, "service": 8,
    "track": 9,
    "trunk_link": 1, "primary_link": 2, "secondary_link": 3, "tertiary_link": 4,
}
NO = {"no", "private"}
SKIP_SERVICE = {"driveway", "parking_aisle", "drive-through", "emergency_access"}
F_ONEWAY, F_LINK, F_DIRT, F_ROUNDABOUT = 1, 2, 4, 16   # 8 is the builder's "fast"
# POI kinds: core::route::PoiKind.
POI = {"fuel": 1, "restaurant": 2, "fast_food": 2, "cafe": 2, "parking": 3}
# Search index: a town's size, which names it as a street's town.
PLACE_SIZE = {"city": 3, "town": 2, "village": 1, "hamlet": 0, "suburb": 0, "neighbourhood": 0}

EDGE = struct.Struct("<IIIIIHBBB3x")   # u v length_dm name_off geom_off geom_n cls mph flags
POI_REC = struct.Struct("<iiBxxxI")    # lat lon kind name_off
HEADER = struct.Struct("<4sIIIIII")    # magic nodes edges geom name_bytes pois osm_date


def mph(tag):
    """A maxspeed tag in mph, 0 if absent or unreadable. OSM's bare number is km/h."""
    if not tag:
        return 0
    m = re.match(r"\s*(\d+)\s*(mph)?", tag)
    if not m:
        return 0
    v = int(m.group(1))
    return min(255, v if m.group(2) else round(v / 1.609344))


def ascii_name(s):
    # The dash's fonts are ASCII.
    return s.encode("ascii", "replace").decode().replace("\0", "")[:63]


def e7(x):
    return int(round(x * 1e7))


def dist_m(a, b):
    la1, lo1, la2, lo2 = (math.radians(v / 1e7) for v in (a[0], a[1], b[0], b[1]))
    x = (lo2 - lo1) * math.cos((la1 + la2) / 2)
    return 6371000.0 * math.hypot(x, la2 - la1)


class Reader(_Handler):
    def __init__(self, bbox=None):
        super().__init__()
        # (south, west, north, east) in degrees: keep only ways with a point
        # inside, and POIs inside. A small map for the bench.
        self.bbox = None if bbox is None else tuple(e7(v) for v in bbox)
        self.ways = []      # (cls, mph, flags, name, [(node_id, lat_e7, lon_e7)], way_id)
        self.restrictions = []   # (only, from_way, via_node, to_way)
        self.via_way = []        # (only, from_way, (via_way, ...), to_way)
        self.pois = []      # (lat_e7, lon_e7, kind, name)
        # For the search index (search.py): house numbers, towns, named places.
        self.addrs = []     # (street, city, number, lat_e7, lon_e7)
        self.places = []    # (name, size, lat_e7, lon_e7): 3 city .. 0 hamlet
        self.named = []     # (name, kind, lat_e7, lon_e7)
        self.skipped = Counter()
        # OSM ids read so far, across files (tagged nodes only: the rest
        # are just way geometry, located per file by pyosmium).
        self.seen = {"n": set(), "w": set(), "r": set()}

    def first(self, kind, oid):
        """True the first time an object is read; again (the next file's copy) False."""
        s = self.seen[kind]
        if oid in s:
            self.skipped["duplicate"] += 1
            return False
        s.add(oid)
        return True

    def inside(self, lat, lon):
        b = self.bbox
        return b is None or (b[0] <= lat <= b[2] and b[1] <= lon <= b[3])

    def _poi(self, tags, lat, lon):
        if not self.inside(lat, lon):
            return
        k = POI.get(tags.get("amenity"))
        if k:
            self.pois.append((lat, lon, k, ascii_name(tags.get("name", ""))))

    def _search(self, t, lat, lon):
        """What the search index wants from anything with a place in it."""
        if not self.inside(lat, lon):
            return
        num = t.get("addr:housenumber")
        if num and "addr:street" in t:
            m = re.match(r"\s*(\d+)", num)
            if m:
                self.addrs.append((ascii_name(t["addr:street"]), ascii_name(t.get("addr:city", "")),
                                   int(m.group(1)), lat, lon))
        name = t.get("name")
        if not name:
            return
        size = PLACE_SIZE.get(t.get("place"))
        if size is not None:
            self.places.append((ascii_name(name), size, lat, lon))
        elif any(k in t for k in ("amenity", "shop", "tourism", "leisure")):
            self.named.append((ascii_name(name), POI.get(t.get("amenity"), 0), lat, lon))

    def node(self, n):
        if not len(n.tags) or not self.first("n", n.id):
            return
        if "amenity" in n.tags:
            self._poi(n.tags, e7(n.location.lat), e7(n.location.lon))
        if len(n.tags):
            self._search(n.tags, e7(n.location.lat), e7(n.location.lon))

    def way(self, w):
        if not self.first("w", w.id):
            return
        t = w.tags
        if ("addr:housenumber" in t or "name" in t) and "highway" not in t and len(w.nodes):
            # A building or an area: its middle, roughly (the mean of its corners).
            try:
                lat = sum(n.lat for n in w.nodes) / len(w.nodes)
                lon = sum(n.lon for n in w.nodes) / len(w.nodes)
                self._search(t, e7(lat), e7(lon))
            except osmium.InvalidLocationError:
                pass
        if "amenity" in t and len(w.nodes) and w.nodes[0].location.valid():
            # shortcut: an area POI sits at its first corner, not its centre;
            # good enough for "nearest fuel".
            self._poi(t, e7(w.nodes[0].lat), e7(w.nodes[0].lon))
        hw = t.get("highway")
        if hw not in CLASS:
            if hw in ("motorway", "motorway_link"):
                self.skipped["motorway"] += 1
            return
        if hw == "service" and t.get("service") in SKIP_SERVICE:
            self.skipped["service"] += 1
            return
        if t.get("access") in NO or t.get("motor_vehicle") in NO \
                or t.get("motorcycle") in NO or t.get("area") == "yes":
            self.skipped["access"] += 1
            return
        try:
            pts = [(n.ref, e7(n.lat), e7(n.lon)) for n in w.nodes]
        except osmium.InvalidLocationError:
            self.skipped["no-location"] += 1
            return
        if len(pts) < 2:
            return
        if self.bbox and not any(self.inside(p[1], p[2]) for p in pts):
            return
        ow = t.get("oneway")
        flags = 0
        if ow == "-1":
            pts.reverse()
            flags |= F_ONEWAY
        elif ow in ("yes", "1", "true") or t.get("junction") in ("roundabout", "circular"):
            flags |= F_ONEWAY
        if t.get("junction") in ("roundabout", "circular"):
            flags |= F_ROUNDABOUT
        if hw.endswith("_link"):
            flags |= F_LINK
        if hw == "track" or t.get("surface") in ("dirt", "gravel", "ground", "unpaved", "compacted"):
            flags |= F_DIRT
        name = t.get("name") or t.get("ref") or ""
        self.ways.append((CLASS[hw], mph(t.get("maxspeed")), flags, ascii_name(name), pts, w.id))

    def relation(self, rel):
        t = rel.tags
        if t.get("type") != "restriction" or not self.first("r", rel.id):
            return
        kind = t.get("restriction:motorcycle") or t.get("restriction:motorcar") or t.get("restriction")
        if not kind or not (kind.startswith("no_") or kind.startswith("only_")):
            return
        if "motorcar" in t.get("except", "") or "motorcycle" in t.get("except", ""):
            return
        frm = [m.ref for m in rel.members if m.type == "w" and m.role == "from"]
        via_n = [m.ref for m in rel.members if m.type == "n" and m.role == "via"]
        via_w = [m.ref for m in rel.members if m.type == "w" and m.role == "via"]
        to = [m.ref for m in rel.members if m.type == "w" and m.role == "to"]
        only = kind.startswith("only_")
        # Several from or to roads (a no_entry from three sides, a no_exit
        # to two) are one rule per pair, exactly, for no_*; only_* allows
        # one way out, so it needs a single `to`.
        pairs = [(f, w) for f in frm for w in to] if to and (not only or len(to) == 1) else []
        if pairs and len(via_n) == 1 and not via_w:
            self.restrictions.extend((only, f, via_n[0], w) for f, w in pairs)
        elif pairs and via_w and not via_n:
            self.via_way.extend((only, f, tuple(via_w), w) for f, w in pairs)
        else:
            self.skipped["restriction-unsupported"] += 1


def osm_date(path):
    try:
        ts = osmium.io.Reader(path, osmium.osm.osm_entity_bits.NOTHING).header() \
            .get("osmosis_replication_timestamp")
        if ts:
            return int(ts[:10].replace("-", ""))
    except Exception:
        pass
    return int(datetime.date.fromtimestamp(os.path.getmtime(path)).strftime("%Y%m%d"))


def build(r):
    use = Counter()
    for _, _, _, _, p, _ in r.ways:
        use[p[0][0]] += 2          # endpoints are always junctions
        use[p[-1][0]] += 2
        for nid, _, _ in p[1:-1]:
            use[nid] += 1
    index = {}                     # osm id -> junction index
    nodes = []
    names = bytearray(b"\0")       # offset 0 is the empty name
    name_off = {"": 0}
    geom = []
    edges = []

    def junction(pt):
        i = index.get(pt[0])
        if i is None:
            i = index[pt[0]] = len(nodes)
            nodes.append((pt[1], pt[2]))
        return i

    def name(s):
        o = name_off.get(s)
        if o is None:
            o = name_off[s] = len(names)
            names.extend(s.encode() + b"\0")
        return o

    def pieces(p):
        start = 0
        for i in range(1, len(p)):
            if use[p[i][0]] < 2 and i < len(p) - 1:
                continue
            seg = p[start:i + 1]
            start = i
            if seg[0][0] != seg[-1][0]:
                yield seg
            elif len(seg) > 2:
                # A loop back to one junction (a cul-de-sac circle): its middle
                # point becomes a junction so the loop is two real edges.
                m = len(seg) // 2
                yield seg[:m + 1]
                yield seg[m:]
            else:
                r.skipped["loop"] += 1

    edge_way = []
    for cls, sp, flags, nm, p, wid in r.ways:
        no = name(nm)
        for seg in pieces(p):
            length = sum(dist_m(a[1:], b[1:]) for a, b in zip(seg, seg[1:]))
            g0 = len(geom)
            geom.extend((q[1], q[2]) for q in seg[1:-1])
            edges.append([junction(seg[0]), junction(seg[-1]), max(1, round(length * 10)),
                          no, g0, len(seg) - 2, cls, sp, flags])   # OSM caps a way at 2000 nodes
            edge_way.append(wid)
    restrict(r, index, nodes, edges, edge_way, geom)
    pois = [(lat, lon, k, name(n)) for lat, lon, k, n in r.pois]
    return nodes, edges, geom, bytes(names), pois


U, V, LEN, NAME, G0, GN, CLS, MPH, FLAGS = range(9)


def restrict(r, index, nodes, edges, edge_way, geom):
    """Turn restrictions by node splitting, so the router needs no turn costs.

    Via a node V: the roads of the `from` way stop arriving at V and arrive
    instead at V', a copy of V on the same spot whose ways out are V's
    minus the forbidden one (no_*) or only the required one (only_*). A
    two-way `from` road becomes two one-way edges so traffic leaving V
    along it is untouched. Everything else at V is as before.

    Via a way W (a short link, one edge from V1 to V2): traffic from the
    `from` way lands on V1', whose ways out are V1's with W swapped for a
    copy W' to V2'; V2' has V2's ways out under the rule.
    """
    oneway = lambda e: e[FLAGS] & F_ONEWAY
    groups = {}
    for only, frm, via, to in r.restrictions:
        vi = index.get(via)
        if vi is None:
            r.skipped["restriction-off-map"] += 1
            continue
        groups.setdefault((vi, frm), []).append((only, to))
    way_ids = {w for _, _, ws, _ in r.via_way for w in ws}
    w_edges = {}
    for i, w in enumerate(edge_way):
        if w in way_ids:
            w_edges.setdefault(w, []).append(i)
    vias = {vi for vi, _ in groups}
    for w, es in w_edges.items():
        for i in es:
            vias.update((edges[i][U], edges[i][V]))
    if not vias:
        return
    inc = {vi: [] for vi in vias}
    for i, e in enumerate(edges):
        for end in (e[U], e[V]):
            if end in inc:
                inc[end].append(i)

    def add(e, way):
        edges.append(e)
        edge_way.append(way)
        for end in (e[U], e[V]):
            if end in inc:
                inc[end].append(len(edges) - 1)
        return len(edges) - 1

    def reversed_geom(e):
        g0 = len(geom)
        geom.extend(reversed(geom[e[G0]:e[G0] + e[GN]]))
        return g0

    def live(vi):
        return [i for i in inc[vi] if vi in (edges[i][U], edges[i][V])]

    def arriving(vi, way):
        return [i for i in live(vi) if edge_way[i] == way and
                (edges[i][V] == vi or (edges[i][U] == vi and not oneway(edges[i])))]

    def copy_out(vi, vp, i):
        """Edge i, as a one-way out of vp in the direction leaving vi; None if it cannot leave vi."""
        e = edges[i]
        if e[U] == vi:
            return add([vp, e[V], e[LEN], e[NAME], e[G0], e[GN], e[CLS], e[MPH], e[FLAGS] | F_ONEWAY], edge_way[i])
        if not oneway(e):
            return add([vp, e[U], e[LEN], e[NAME], reversed_geom(e), e[GN], e[CLS], e[MPH], e[FLAGS] | F_ONEWAY],
                       edge_way[i])
        return None

    def redirect(vi, vp, ins):
        """The `ins` roads arrive at vp instead of vi; leaving vi along them stays possible."""
        for i in ins:
            e = edges[i]
            if e[V] == vi:
                if not oneway(e):
                    add([vi, e[U], e[LEN], e[NAME], reversed_geom(e), e[GN], e[CLS], e[MPH], e[FLAGS] | F_ONEWAY],
                        edge_way[i])
                e[V] = vp
                e[FLAGS] |= F_ONEWAY
            else:
                add([e[V], vp, e[LEN], e[NAME], reversed_geom(e), e[GN], e[CLS], e[MPH], e[FLAGS] | F_ONEWAY],
                    edge_way[i])
                e[FLAGS] |= F_ONEWAY

    def rule(rules):
        only = {to for o, to in rules if o}
        banned = {to for o, to in rules if not o}
        return (lambda w: w in only) if only else (lambda w: w not in banned)

    def copy_node(vi):
        nodes.append(nodes[vi])
        return len(nodes) - 1

    mapped = set(edge_way)
    for (vi, frm), rules in groups.items():
        ins = arriving(vi, frm)
        if not ins:
            # A `from` road left off the map (an interstate ramp) cannot be
            # driven onto the turn: nothing lost. Otherwise it is a road on
            # the map that does not reach the junction, or only leaves it.
            r.skipped["restriction-from-off-map" if frm not in mapped else "restriction-no-from"] += 1
            continue
        allowed = rule(rules)
        outs = live(vi)
        vp = copy_node(vi)
        for i in outs:
            if allowed(edge_way[i]):
                copy_out(vi, vp, i)
        redirect(vi, vp, ins)
        r.skipped["restricted-junctions"] += 1

    for only, frm, ws, to in r.via_way:
        # The via ways, each one edge, chained end to end from where the
        # `from` way arrives to where the `to` way leaves.
        if any(len(w_edges.get(w, [])) != 1 for w in ws):
            r.skipped["restriction-via-way-long"] += 1     # a via way split by a junction
            continue
        chain_e = [w_edges[w][0] for w in ws]
        if any(e not in inc for i in chain_e for e in (edges[i][U], edges[i][V])):
            # A node restriction already moved a link's end to a copy; the
            # two rules on one link are not combined.
            r.skipped["restriction-via-way-overlap"] += 1
            continue
        start = [v for i in chain_e for v in (edges[i][U], edges[i][V]) if arriving(v, frm)]
        path, left = None, set(chain_e)
        if len(start) == 1:
            path, at = [start[0]], start[0]
            while left:
                nxt = [i for i in left if at in (edges[i][U], edges[i][V])]
                if len(nxt) != 1:
                    path = None
                    break
                i = nxt[0]
                left.discard(i)
                at = edges[i][V] if edges[i][U] == at else edges[i][U]
                if edges[i][U] != path[-1] and oneway(edges[i]):
                    path = None                          # the chain runs against a one-way
                    break
                path += [i, at]
        if not path or not any(edge_way[i] == to for i in live(path[-1])):
            r.skipped["restriction-via-way-unjoined"] += 1
            continue
        # path: v0, e1, v1, e2, ..., vk. Copies v0'..vk' carry only this
        # traffic: at each it may leave the chain anywhere (that is not the
        # restricted move), and at vk only as the rule allows.
        vs, es = path[0::2], path[1::2]
        cp = [copy_node(v) for v in vs]
        allowed = rule([(only, to)])
        for k, v in enumerate(vs):
            ahead = es[k] if k < len(es) else None
            behind = es[k - 1] if k > 0 else None
            for i in live(v):
                if i in (ahead, behind):
                    continue
                if k == len(vs) - 1 and not allowed(edge_way[i]):
                    continue
                copy_out(v, cp[k], i)
            if ahead is not None:
                e = edges[ahead]
                g0 = e[G0] if e[U] == v else reversed_geom(e)
                add([cp[k], cp[k + 1], e[LEN], e[NAME], g0, e[GN], e[CLS], e[MPH], e[FLAGS] | F_ONEWAY],
                    edge_way[ahead])
        redirect(vs[0], cp[0], arriving(vs[0], frm))
        r.skipped["restricted-via-ways"] += 1


def write(path, nodes, edges, geom, names, pois, date):
    with open(path, "wb") as f:
        f.write(HEADER.pack(b"RGI1", len(nodes), len(edges), len(geom), len(names), len(pois), date))
        f.write(b"".join(struct.pack("<ii", *n) for n in nodes))
        f.write(b"".join(EDGE.pack(*e) for e in edges))
        f.write(b"".join(struct.pack("<ii", *g) for g in geom))
        f.write(names)
        f.write(b"".join(POI_REC.pack(*p) for p in pois))


SXI_HEAD = struct.Struct("<4sIIII")       # magic entries nums name_bytes osm_date
SXI_ENTRY = struct.Struct("<BBHIIiiII")    # core::route::PlaceEntry
SXI_NUM = struct.Struct("<Iii")            # core::route::HouseNumber
STREET, TOWN, PLACE = 1, 2, 3              # core::route::Place


def norm(s):
    """core::route::normalize, for grouping: same street, same key."""
    return " ".join("".join(c if c.isalnum() and c.isascii() else " " for c in s.upper()).split())


def write_search(path, r, date):
    """The search index's raw entries (.sxi) for rgf_build --search."""
    # Towns, to name a street's town where its addresses do not.
    cell = 0.1
    grid = {}
    for name, size, lat, lon in r.places:
        if size >= 1:
            grid.setdefault((int(lat / 1e7 / cell), int(lon / 1e7 / cell)), []).append((name, size, lat, lon))

    # How far a place's name reaches: a city's covers a village inside it
    # (Cammack Village is in Little Rock), and past them all, nothing.
    reach = {3: 10000, 2: 5000, 1: 2000}

    def town(lat, lon):
        cy, cx = int(lat / 1e7 / cell), int(lon / 1e7 / cell)
        best, key = "", None
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                for name, size, plat, plon in grid.get((cy + dy, cx + dx), ()):
                    d = dist_m((lat, lon), (plat, plon))
                    if d <= reach[size] and (key is None or (-size, d) < key):
                        best, key = name, (-size, d)
        return best

    streets = {}   # (norm name, norm town) -> [display name, display town, [points], {number: point}]
    for _, _, _, name, pts, _ in r.ways:
        if not name:
            continue
        mid = pts[len(pts) // 2]
        t = town(mid[1], mid[2])
        s = streets.setdefault((norm(name), norm(t)), [name, t, [], {}])
        s[2].append((mid[1], mid[2]))
    for street, city, num, lat, lon in r.addrs:
        t = city or town(lat, lon)
        key = (norm(street), norm(t))
        s = streets.get(key)
        if s is None:
            s = streets[key] = [street, t, [], {}]
        s[3].setdefault(num, (lat, lon))
        if not s[2]:
            s[2].append((lat, lon))

    names = bytearray(b"\0")
    offs = {"": 0}

    def name_off(x):
        if x not in offs:
            offs[x] = len(names)
            names.extend(x.encode("ascii", "replace") + b"\0")
        return offs[x]

    entries, nums = [], []
    for name, t, pts, numbers in streets.values():
        # The point among its pieces nearest their middle: on the street.
        my = sum(p[0] for p in pts) / len(pts)
        mx = sum(p[1] for p in pts) / len(pts)
        lat, lon = min(pts, key=lambda p: (p[0] - my) ** 2 + (p[1] - mx) ** 2)
        n0 = len(nums)
        nums.extend((n, plat, plon) for n, (plat, plon) in sorted(numbers.items()))
        entries.append((STREET, 0, 0, name_off(name), name_off(t), lat, lon, n0, len(numbers)))
    for name, size, lat, lon in r.places:
        entries.append((TOWN, size, 0, name_off(name), 0, lat, lon, 0, 0))
    for name, kind, lat, lon in r.named:
        entries.append((PLACE, kind, 0, name_off(name), name_off(town(lat, lon)), lat, lon, 0, 0))
    with open(path, "wb") as f:
        f.write(SXI_HEAD.pack(b"SXI1", len(entries), len(nums), len(names), date))
        f.write(b"".join(SXI_ENTRY.pack(*e) for e in entries))
        f.write(b"".join(SXI_NUM.pack(*n) for n in nums))
        f.write(names)
    return len(entries)


def main():
    if osmium is None:
        sys.exit("extract.py needs pyosmium: pip install osmium")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pbf", nargs="+", help="one or more .osm.pbf extracts, made into one map")
    ap.add_argument("out")
    ap.add_argument("--bbox", type=float, nargs=4, metavar=("S", "W", "N", "E"),
                    help="only roads with a point in this box (degrees): a small map for the bench")
    a = ap.parse_args()
    r = Reader(a.bbox)
    for pbf in a.pbf:
        r.apply_file(pbf, locations=True)
    nodes, edges, geom, names, pois = build(r)
    date = min(osm_date(p) for p in a.pbf)     # the oldest data in it
    write(a.out, nodes, edges, geom, names, pois, date)
    sxi = os.path.splitext(a.out)[0] + ".sxi"
    n_search = write_search(sxi, r, date)
    print(f"{a.out}: OSM {date}, {len(nodes):,} junctions, {len(edges):,} edges, "
          f"{len(geom):,} shape points, {len(pois):,} POIs, {len(names):,} B names; "
          f"skipped {dict(r.skipped)}; {sxi}: {n_search:,} search entries", file=sys.stderr)


if __name__ == "__main__":
    main()
