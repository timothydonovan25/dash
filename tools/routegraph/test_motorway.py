#!/usr/bin/env python3
"""Interstates are on the map, and one way unless OSM says otherwise.

    python3 tools/routegraph/test_motorway.py

OSM leaves `oneway` off most motorways because a motorway implies it; a
map that took them as two-way would route against the traffic.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402

OSM = """<?xml version="1.0"?>
<osm version="0.6">
<node id="1" version="1" lat="36.0" lon="-94.0"/>
<node id="2" version="1" lat="36.0" lon="-93.99"/>
<node id="3" version="1" lat="36.01" lon="-93.99"/>
<way id="10" version="1"><nd ref="1"/><nd ref="2"/><tag k="highway" v="motorway"/><tag k="ref" v="I 49"/></way>
<way id="11" version="1"><nd ref="2"/><nd ref="3"/><tag k="highway" v="motorway_link"/><tag k="oneway" v="no"/></way>
<way id="12" version="1"><nd ref="3"/><nd ref="1"/><tag k="highway" v="residential"/></way>
</osm>
"""


def main():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "m.osm")
        with open(p, "w") as f:
            f.write(OSM)
        r = extract.Reader()
        r.apply_file(p, locations=True)
    by_id = {w[5]: w for w in r.ways}
    checks = [
        ("all three ways kept", sorted(by_id) == [10, 11, 12]),
        ("the motorway is class 10", by_id[10][0] == 10 and by_id[11][0] == 10),
        ("an untagged motorway is one way", by_id[10][2] & extract.F_ONEWAY),
        ("oneway=no is two way", not by_id[11][2] & extract.F_ONEWAY),
        ("the link is a link", by_id[11][2] & extract.F_LINK),
        ("a residential street is two way", not by_id[12][2] & extract.F_ONEWAY),
    ]
    fails = 0
    for what, ok in checks:
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} {what}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
