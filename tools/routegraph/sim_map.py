#!/usr/bin/env python3
"""A made-up town where the simulator's bike rides (42.5 N, 83.1 W), so
CRUISE -> NAV has a map in the sim (docs/navigation.md).

    python3 tools/routegraph/sim_map.py build-host/rgf_build [sim_card]

Writes sim_card/route/map.rgf (a 9 x 9 grid of streets 200 m apart, one
divided highway along the south edge so the 65 mph question comes up, a
dirt track on the diagonal), sim_card/route/search.idx (its streets with
house numbers 100 a block, the town "Simtown", two places) and
sim_card/route/destinations.csv. No OSM
download and no pyosmium: it writes extract.py's .rgi itself.
"""
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract import CLASS, F_DIRT, F_ONEWAY, SXI_ENTRY, SXI_HEAD, SXI_NUM, write  # noqa: E402

LAT0, LON0 = 42.5, -83.1          # sim/sim_bike.cpp's origin
N = 9
STEP_M = 200.0
DLAT = STEP_M / 111195.0
DLON = STEP_M / (111195.0 * math.cos(math.radians(LAT0)))


def e7(x):
    return int(round(x * 1e7))


def main():
    rgf_build = sys.argv[1] if len(sys.argv) > 1 else "build-host/rgf_build"
    card = sys.argv[2] if len(sys.argv) > 2 else "sim_card"
    # Centred on the bike's origin.
    nodes = [(e7(LAT0 + (r - N // 2) * DLAT), e7(LON0 + (c - N // 2) * DLON)) for r in range(N) for c in range(N)]
    names = bytearray(b"\0")
    offs = {}

    def name(s):
        if s not in offs:
            offs[s] = len(names)
            names.extend(s.encode() + b"\0")
        return offs[s]

    edges = []

    def road(u, v, cls, nm, flags=0, mph=0):
        edges.append([u, v, int(STEP_M * 10 * (1.414 if cls == "track" else 1)), name(nm), 0, 0,
                      CLASS[cls], mph, flags])

    nid = lambda r, c: r * N + c
    streets = ["Maple", "Oak", "Elm", "Pine", "Cedar", "Birch", "Walnut", "Spruce", "Ash"]
    for r in range(N):
        for c in range(N - 1):
            if r == 0:
                road(nid(r, c), nid(r, c + 1), "trunk", "M-59")          # untagged divided highway
            else:
                road(nid(r, c), nid(r, c + 1), "residential", f"{streets[r]} St")
    for c in range(N):
        for r in range(N - 1):
            ow = F_ONEWAY if c == 4 else 0                               # Main St runs north only
            road(nid(r, c), nid(r + 1, c), "tertiary" if c == 4 else "residential",
                 "Main St" if c == 4 else f"{c + 1}th Ave", ow)
    for k in range(N - 1):
        road(nid(k, k), nid(k + 1, k + 1), "track", "Gravel Rd", F_DIRT)
    pois = [(nodes[nid(2, 6)][0], nodes[nid(2, 6)][1], 1, name("Speedway")),
            (nodes[nid(6, 2)][0], nodes[nid(6, 2)][1], 2, name("Diner"))]
    os.makedirs(os.path.join(card, "route"), exist_ok=True)
    rgi = os.path.join(card, "route", "sim.rgi")
    write(rgi, nodes, edges, [], bytes(names), pois, 20261103)
    subprocess.run([rgf_build, rgi, os.path.join(card, "route", "map.rgf")], check=True)
    os.remove(rgi)
    # The search index: every street with house numbers 100 a block along
    # it, the town, and its two places.
    snames = bytearray(b"\0")
    soffs = {"": 0}

    def sname(x):
        if x not in soffs:
            soffs[x] = len(snames)
            snames.extend(x.encode() + b"\0")
        return soffs[x]

    entries, nums = [], []
    for r in range(1, N):
        n0 = len(nums)
        nums.extend((100 * (c + 1), *nodes[nid(r, c)]) for c in range(N))
        mid = nodes[nid(r, N // 2)]
        entries.append((1, 0, 0, sname(f"{streets[r]} Street"), sname("Simtown"), *mid, n0, N))
    for c in range(N):
        n0 = len(nums)
        nums.extend((100 * (r + 1), *nodes[nid(r, c)]) for r in range(N))
        mid = nodes[nid(N // 2, c)]
        nm = "Main Street" if c == 4 else f"{c + 1}th Avenue"
        entries.append((1, 0, 0, sname(nm), sname("Simtown"), *mid, n0, N))
    entries.append((2, 2, 0, sname("Simtown"), 0, *nodes[nid(N // 2, N // 2)], 0, 0))
    entries.append((3, 1, 0, sname("Speedway"), sname("Simtown"), *nodes[nid(2, 6)], 0, 0))
    entries.append((3, 2, 0, sname("Diner"), sname("Simtown"), *nodes[nid(6, 2)], 0, 0))
    sxi = os.path.join(card, "route", "sim.sxi")
    with open(sxi, "wb") as f:
        f.write(SXI_HEAD.pack(b"SXI1", len(entries), len(nums), len(snames), 20261103))
        f.write(b"".join(SXI_ENTRY.pack(*e) for e in entries))
        f.write(b"".join(SXI_NUM.pack(*n) for n in nums))
        f.write(snames)
    subprocess.run([rgf_build, "--search", sxi, os.path.join(card, "route", "search.idx")], check=True)
    os.remove(sxi)
    far = nodes[nid(N - 1, N - 1)]
    with open(os.path.join(card, "route", "destinations.csv"), "w") as f:
        f.write("# the sim's town (tools/routegraph/sim_map.py)\n")
        f.write(f"North End,{far[0] / 1e7:.6f},{far[1] / 1e7:.6f}\n")
        sw = nodes[nid(0, N - 1)]
        f.write(f"Highway East,{sw[0] / 1e7:.6f},{sw[1] / 1e7:.6f}\n")
    print(f"{card}/route/map.rgf and destinations.csv written")


if __name__ == "__main__":
    main()
