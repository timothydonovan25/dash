#!/usr/bin/env python3
"""Turn restrictions, end to end: extract.py on a tiny .osm, rgf_build, and
the dash's own query (rgf_build --route).

    python3 tools/routegraph/test_restrictions.py build-host/rgf_build

Needs pyosmium, like extract.py. A crossroads at 36.0,-94.0 with South,
North, East and West Rd, and a Back Rd from the north end round to the
west end. Heading north on South Rd, a left onto West Rd is the short way
west; the restriction forbids it, so the route goes on north and round.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402

NODES = {1: (36.0, -94.0), 2: (35.998, -94.0), 3: (36.002, -94.0), 4: (36.0, -93.9975),
         5: (36.0, -94.0025), 6: (36.002, -94.0025)}
WAYS = {10: ("South Rd", [2, 1]), 11: ("North Rd", [1, 3]), 12: ("East Rd", [1, 4]),
        13: ("West Rd", [1, 5]), 14: ("Back Rd", [3, 6, 5])}


def osm(restriction, nodes=None, ways=None, via="node"):
    nodes = nodes or NODES
    ways = ways or WAYS
    out = ['<?xml version="1.0"?>', '<osm version="0.6">']
    for i, (lat, lon) in nodes.items():
        out.append(f'<node id="{i}" version="1" lat="{lat}" lon="{lon}"/>')
    for i, (name, refs) in ways.items():
        out.append(f'<way id="{i}" version="1">' + "".join(f'<nd ref="{r}"/>' for r in refs) +
                   f'<tag k="highway" v="residential"/><tag k="name" v="{name}"/></way>')
    if restriction:
        kind, frm, to = restriction[:3]
        via_xml = (f'<member type="way" ref="{restriction[3]}" role="via"/>' if via == "way"
                   else '<member type="node" ref="1" role="via"/>')
        out.append('<relation id="100" version="1">'
                   f'<member type="way" ref="{frm}" role="from"/>' + via_xml +
                   f'<member type="way" ref="{to}" role="to"/>'
                   f'<tag k="type" v="restriction"/><tag k="restriction" v="{kind}"/></relation>')
    out.append("</osm>")
    return "\n".join(out)


def route(rgf_build, d, restriction, nodes=None, ways=None, via="node", a="35.9985,-94.0", b="36.0,-94.002"):
    src = os.path.join(d, "t.osm")
    with open(src, "w") as f:
        f.write(osm(restriction, nodes, ways, via))
    r = extract.Reader()
    r.apply_file(src, locations=True)
    nodes, edges, geom, names, pois = extract.build(r)
    rgi, rgf = os.path.join(d, "t.rgi"), os.path.join(d, "t.rgf")
    extract.write(rgi, nodes, edges, geom, names, pois, 20261103)
    subprocess.run([rgf_build, rgi, rgf], check=True, capture_output=True)
    out = subprocess.run([rgf_build, "--rgf", rgf, "--route", a, b],
                         capture_output=True, text=True).stdout
    miles = float(re.match(r"ok: ([\d.]+) mi", out).group(1))
    return miles, out, len(nodes)


def main():
    rgf_build = sys.argv[1] if len(sys.argv) > 1 else "build-host/rgf_build"
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print("  FAIL", what)

    with tempfile.TemporaryDirectory() as d:
        free, out, n_free = route(rgf_build, d, None)
        check("left" in out and "West Rd" in out, "unrestricted: a left onto West Rd\n" + out)
        no_left, out, n = route(rgf_build, d, ("no_left_turn", 10, 13))
        check(no_left > free + 0.2, f"no_left_turn: round the block ({no_left} vs {free})\n" + out)
        # Round the block, or back down North Rd and right: either is legal
        # (the router has no U-turn cost), but never the left at the cross.
        check(not re.search(r"left\s+-?\d+ deg\s+West Rd", out), "no_left_turn: no left onto West Rd\n" + out)
        check(n == n_free + 1, "one junction split")
        only, out, _ = route(rgf_build, d, ("only_straight_on", 10, 11))
        check(abs(only - no_left) < 0.01, f"only_straight_on: the same way round ({only})\n" + out)
        # Two from roads in one relation (a no_entry, tagged once for both
        # sides): from South Rd, round the block as before.
        two = osm(None).replace("</osm>",
              '<relation id="102" version="1"><member type="way" ref="10" role="from"/>'
              '<member type="way" ref="12" role="from"/><member type="node" ref="1" role="via"/>'
              '<member type="way" ref="13" role="to"/><tag k="type" v="restriction"/>'
              '<tag k="restriction" v="no_left_turn"/></relation></osm>')
        with open(os.path.join(d, "two.osm"), "w") as f:
            f.write(two)
        rd = extract.Reader()
        rd.apply_file(os.path.join(d, "two.osm"), locations=True)
        check(len(rd.restrictions) == 2 and not rd.skipped["restriction-unsupported"],
              f"two from roads: one rule each ({rd.restrictions}, {dict(rd.skipped)})")
        nodes2, edges2, geom2, names2, pois2 = extract.build(rd)
        rgi2, rgf2 = os.path.join(d, "two.rgi"), os.path.join(d, "two.rgf")
        extract.write(rgi2, nodes2, edges2, geom2, names2, pois2, 20261103)
        subprocess.run([rgf_build, rgi2, rgf2], check=True, capture_output=True)
        out = subprocess.run([rgf_build, "--rgf", rgf2, "--route", "35.9985,-94.0", "36.0,-94.002"],
                             capture_output=True, text=True).stdout
        both = float(re.match(r"ok: ([\d.]+) mi", out).group(1))
        check(abs(both - no_left) < 0.01, f"two from roads: round the block from South Rd ({both})\n" + out)
        # A restriction on another approach leaves this one alone.
        other, _, _ = route(rgf_build, d, ("no_left_turn", 12, 11))
        check(abs(other - free) < 0.01, f"another approach's restriction: still the short way ({other})")
    # Via a way: South Rd north to the crossroads, east along a 50 m Link
    # to a T with Cross St, and no left (north) onto Cross St from South Rd
    # by way of the Link. The way round: on north, along Top Rd, down.
    vn = dict(NODES)
    vn.update({7: (36.0, -93.99945), 8: (36.002, -93.99945)})
    vw = {10: ("South Rd", [2, 1]), 11: ("North Rd", [1, 3]), 15: ("Link", [1, 7]),
          16: ("Cross St", [7, 8]), 17: ("Top Rd", [3, 8]), 18: ("East Rd", [7, 4])}
    goal = "36.0018,-93.99945"
    with tempfile.TemporaryDirectory() as d:
        free, out, n_free = route(rgf_build, d, None, vn, vw, b=goal)
        check("Cross St" in out and "Link" in out, "via way, unrestricted: Link then Cross St\n" + out)
        banned, out, n = route(rgf_build, d, ("no_left_turn", 10, 16, 15), vn, vw, "way", b=goal)
        check(banned > free and "Top Rd" in out and "Link" not in out,
              f"via way: round by Top Rd, not the Link ({banned} vs {free})\n" + out)
        check(n == n_free + 2, f"via way: two junctions copied ({n} vs {n_free})")
        # From East Rd the same left across nothing is still fine.
        other, _, _ = route(rgf_build, d, ("no_left_turn", 10, 16, 15), vn, vw, "way",
                            a="36.0,-93.9976", b=goal)
        free_e, _, _ = route(rgf_build, d, None, vn, vw, a="36.0,-93.9976", b=goal)
        check(abs(other - free_e) < 0.01, f"via way: other approaches untouched ({other} vs {free_e})")
    # A chain of two via ways (how a U-turn across a divided road's median
    # is tagged): South Rd, Link A, Link B, then Cross St north is banned.
    cn = dict(NODES)
    cn.update({7: (36.0, -93.99972), 9: (36.0, -93.99945), 10: (36.002, -93.99945)})
    cw = {10: ("South Rd", [2, 1]), 11: ("North Rd", [1, 3]), 15: ("Link A", [1, 7]),
          19: ("Link B", [7, 9]), 16: ("Cross St", [9, 10]), 17: ("Top Rd", [3, 10]),
          18: ("East Rd", [9, 4])}
    goal = "36.0018,-93.99945"
    with tempfile.TemporaryDirectory() as d:
        free, out, n_free = route(rgf_build, d, None, cn, cw, b=goal)
        check("Cross St" in out and "Top Rd" not in out, "chain, unrestricted: along the links\n" + out)
        with open(os.path.join(d, "t.osm"), "w") as f:
            f.write(osm(None, cn, cw))
        # The relation by hand: two via ways.
        xml = osm(None, cn, cw).replace("</osm>",
              '<relation id="101" version="1"><member type="way" ref="10" role="from"/>'
              '<member type="way" ref="15" role="via"/><member type="way" ref="19" role="via"/>'
              '<member type="way" ref="16" role="to"/><tag k="type" v="restriction"/>'
              '<tag k="restriction" v="no_left_turn"/></relation></osm>')
        src = os.path.join(d, "t.osm")
        with open(src, "w") as f:
            f.write(xml)
        rd = extract.Reader()
        rd.apply_file(src, locations=True)
        nodes, edges, geom, names, pois = extract.build(rd)
        rgi, rgf = os.path.join(d, "c.rgi"), os.path.join(d, "c.rgf")
        extract.write(rgi, nodes, edges, geom, names, pois, 20261103)
        subprocess.run([rgf_build, rgi, rgf], check=True, capture_output=True)
        out = subprocess.run([rgf_build, "--rgf", rgf, "--route", "35.9985,-94.0", goal],
                             capture_output=True, text=True).stdout
        check(rd.skipped["restricted-via-ways"] == 1, f"chain: applied ({dict(rd.skipped)})")
        check(len(nodes) == n_free + 3, f"chain: three junctions copied ({len(nodes)} vs {n_free})")
        check("Top Rd" in out, "chain: round by Top Rd\n" + out)
    print("PASS restrictions" if not fails else f"FAIL restrictions — {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
