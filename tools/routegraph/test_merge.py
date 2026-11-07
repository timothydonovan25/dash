#!/usr/bin/env python3
"""Two overlapping extracts make one map, as two states do at their border.

    python3 tools/routegraph/test_merge.py build-host/rgf_build

test_restrictions.py's crossroads, split in two files that both carry
North Rd (the "border" road), its nodes and the restriction: read
together they must give exactly the one-file map, and a route from a road
only in the first file to one only in the second must cross.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402
from test_restrictions import NODES, WAYS, osm  # noqa: E402

RESTRICTION = ("no_left_turn", 10, 13)


def build(paths):
    r = extract.Reader()
    for p in paths:
        r.apply_file(p, locations=True)
    return r, extract.build(r)


def main():
    rgf_build = sys.argv[1] if len(sys.argv) > 1 else "build-host/rgf_build"
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        whole = os.path.join(d, "whole.osm")
        south = os.path.join(d, "south.osm")      # South, North, West, Back Rd
        east = os.path.join(d, "east.osm")        # North (again) and East Rd
        with open(whole, "w") as f:
            f.write(osm(RESTRICTION))
        with open(south, "w") as f:
            f.write(osm(RESTRICTION, ways={k: v for k, v in WAYS.items() if k != 12}))
        with open(east, "w") as f:
            f.write(osm(RESTRICTION, nodes={k: NODES[k] for k in (1, 3, 4)},
                        ways={k: WAYS[k] for k in (11, 12)}))
        one, (n1, e1, g1, _, _) = build([whole])
        two, (n2, e2, g2, names, pois) = build([south, east])
        for what, a, b in (("junctions", len(n1), len(n2)), ("edges", len(e1), len(e2)),
                           ("shape points", len(g1), len(g2)),
                           ("restrictions", len(one.restrictions), len(two.restrictions))):
            ok = a == b
            fails += not ok
            print(f"{'PASS' if ok else 'FAIL'} {what}: one file {a}, two files {b}")
        ok = two.skipped["duplicate"] == 2      # North Rd and the relation (nodes 1, 3 are untagged)
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} duplicates skipped: {two.skipped['duplicate']}")
        rgi, rgf = os.path.join(d, "t.rgi"), os.path.join(d, "t.rgf")
        extract.write(rgi, n2, e2, g2, names, pois, 20261103)
        subprocess.run([rgf_build, rgi, rgf], check=True, capture_output=True)
        out = subprocess.run([rgf_build, "--rgf", rgf, "--route", "35.9985,-94.0", "36.0,-93.998"],
                             capture_output=True, text=True).stdout
        ok = "East Rd" in out
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} South Rd (first file) to East Rd (second) routes across")
        if not ok:
            print(out)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
