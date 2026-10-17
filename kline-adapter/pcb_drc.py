"""Connectivity and clearance check for an EasyEDA Std PCB (JSON source).

Builds copper items per layer (tracks, vias, pads, and pour fills including
their cutouts), joins items that touch, and reports:
  - nets whose pads are not all joined (unrouted),
  - copper of different nets touching (short),
  - copper of different nets closer than CLEAR_MM.

Round pads are circles and pour cutouts are holes, so neither produces false
shorts. Oval and polygon pads use their outline.

    python3 -I pcb_drc.py [pcb.json] [clearance_mm]

With no file, it checks the newest PCB_PCB_*.json in ~/Downloads.
"""
import collections
import json
import math
import re
import sys

U = 0.254  # mm per EasyEDA unit


def seg_pt(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def seg_seg(a, b, c, e):
    def ccw(p, q, r):
        return (r[1] - p[1]) * (q[0] - p[0]) - (q[1] - p[1]) * (r[0] - p[0])
    if ccw(a, b, c) * ccw(a, b, e) < 0 and ccw(c, e, a) * ccw(c, e, b) < 0:
        return 0
    return min(seg_pt(a, c, e), seg_pt(b, c, e), seg_pt(c, a, b), seg_pt(e, a, b))


def inside(p, rings):
    """Even-odd over all rings, so cutouts are holes."""
    x, y = p
    c = False
    for poly in rings:
        for i in range(len(poly)):
            x1, y1 = poly[i]; x2, y2 = poly[i - 1]
            if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                c = not c
    return c


def edges(rings):
    for r in rings:
        for i in range(len(r)):
            yield r[i], r[i - 1]


class Item:
    __slots__ = ("kind", "layers", "net", "rings", "segs", "circle", "name", "bbox")

    def __init__(self, kind, layers, net, name, rings=None, segs=None, circle=None):
        self.kind, self.layers, self.net, self.name = kind, layers, net, name
        self.rings, self.segs, self.circle = rings, segs, circle
        if rings:
            xs = [p[0] for r in rings for p in r]; ys = [p[1] for r in rings for p in r]
            self.bbox = (min(xs), min(ys), max(xs), max(ys))
        elif segs:
            (a, b, hw) = segs
            self.bbox = (min(a[0], b[0]) - hw, min(a[1], b[1]) - hw, max(a[0], b[0]) + hw, max(a[1], b[1]) + hw)
        else:
            (c, r) = circle
            self.bbox = (c[0] - r, c[1] - r, c[0] + r, c[1] + r)


def dist(A, B):
    """Gap between two copper items (<= 0 means touching)."""
    if A.rings is None and B.rings is not None:
        A, B = B, A
    if A.rings is not None:
        R = A.rings
        if B.rings is not None:
            if any(inside(q, R) for r in B.rings for q in r[:1]) or any(inside(q, B.rings) for r in R for q in r[:1]):
                return 0
            return min(seg_seg(a, b, c, e) for a, b in edges(R) for c, e in edges(B.rings))
        if B.segs is not None:
            a, b, hw = B.segs
            if inside(a, R) or inside(b, R):
                return -hw
            return min(seg_seg(a, b, c, e) for c, e in edges(R)) - hw
        c, r = B.circle
        if inside(c, R):
            return -r
        return min(seg_pt(c, a, b) for a, b in edges(R)) - r
    if A.segs is not None and B.segs is not None:
        return seg_seg(A.segs[0], A.segs[1], B.segs[0], B.segs[1]) - A.segs[2] - B.segs[2]
    if A.circle is not None and B.circle is not None:
        return math.hypot(A.circle[0][0] - B.circle[0][0], A.circle[0][1] - B.circle[0][1]) - A.circle[1] - B.circle[1]
    S, C = (A, B) if A.segs is not None else (B, A)
    return seg_pt(C.circle[0], S.segs[0], S.segs[1]) - S.segs[2] - C.circle[1]


def load(path):
    d = json.load(open(path))
    items = []
    for s in d["shape"]:
        t = s.split("~")
        if t[0] == "TRACK" and t[2] in ("1", "2"):
            v = [float(x) for x in t[4].split()]
            pts = list(zip(v[0::2], v[1::2]))
            for a, b in zip(pts, pts[1:]):
                items.append(Item("T", {t[2]}, t[3], "track " + t[5], segs=(a, b, float(t[1]) / 2)))
        elif t[0] == "VIA":
            items.append(Item("V", {"1", "2"}, t[4], "via " + t[6],
                              circle=((float(t[1]), float(t[2])), float(t[3]) / 2)))
        elif t[0] == "COPPERAREA":
            fill = json.loads([x for x in t if x.startswith("[[")][0])
            for isl in fill:
                rings = []
                for ring in isl:
                    for sp in re.split(r"(?=M)", ring):
                        v = [float(x) for x in re.findall(r"-?\d+\.?\d*", sp)]
                        if len(v) >= 6:
                            rings.append(list(zip(v[0::2], v[1::2])))
                items.append(Item("A", {t[2]}, t[3], "pour L" + t[2], rings=rings))
        elif t[0] == "LIB":
            sub = s.split("#@$")
            ref = [p.split("~")[10] for p in sub if p.startswith("TEXT~P")][0]
            for p in sub:
                q = p.split("~")
                if q[0] != "PAD":
                    continue
                layers = {"1"} if q[6] == "1" else {"2"} if q[6] == "2" else {"1", "2"}
                name = f"pad {ref}-{q[8]}"
                x, y, w, h = map(float, q[2:6])
                if q[1] == "ELLIPSE" and abs(w - h) < 1e-6:
                    items.append(Item("P", layers, q[7], name, circle=((x, y), w / 2)))
                    continue
                v = [float(z) for z in q[10].split()] if q[10].strip() else []
                if len(v) >= 6:
                    poly = list(zip(v[0::2], v[1::2]))
                else:
                    r = math.radians(float(q[11] or 0))
                    poly = [(x + dx * math.cos(r) - dy * math.sin(r), y + dx * math.sin(r) + dy * math.cos(r))
                            for dx, dy in ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2))]
                items.append(Item("P", layers, q[7], name, rings=[poly]))
    return items


def main(path, clear_mm=0.127):
    items = load(path)
    parent = list(range(len(items)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    lim = clear_mm / U
    problems = []
    for i in range(len(items)):
        A = items[i]
        for j in range(i + 1, len(items)):
            B = items[j]
            if not (A.layers & B.layers):
                continue
            if (A.bbox[0] - lim > B.bbox[2] or B.bbox[0] - lim > A.bbox[2] or
                    A.bbox[1] - lim > B.bbox[3] or B.bbox[1] - lim > A.bbox[3]):
                continue
            same_part = A.kind == B.kind == "P" and A.name.split("-")[0] == B.name.split("-")[0]
            dd = dist(A, B)
            if dd <= 1e-6:
                if A.net == B.net:
                    parent[find(i)] = find(j)
                elif not same_part:
                    problems.append(("SHORT", A.net, B.net, A.name, B.name, dd * U))
            elif A.net != B.net and dd * U < clear_mm and not same_part:
                problems.append(("CLEARANCE", A.net, B.net, A.name, B.name, dd * U))

    groups = collections.defaultdict(lambda: collections.defaultdict(list))
    for i, it in enumerate(items):
        if it.kind == "P" and it.net:
            groups[it.net][find(i)].append(it.name[4:])
    unrouted = {n: [sorted(v) for v in g.values()] for n, g in groups.items() if len(g) > 1}
    for i, it in enumerate(items):
        if it.kind == "A":
            tied = any(find(j) == find(i) for j, o in enumerate(items) if o.kind == "P" and o.net == it.net)
            print(f"{it.name} ({it.net}): {'tied to its pads' if tied else 'FLOATING'}")
    for n, g in sorted(unrouted.items()):
        print("UNROUTED", n, g)
    for p in problems:
        print(f"{p[0]:9} {p[1]} / {p[2]}  {p[3]} <-> {p[4]}  {p[5]:.3f} mm")
    print(f"{len(groups)} nets with pads, {len(unrouted)} unrouted, "
          f"{sum(p[0] == 'SHORT' for p in problems)} shorts, "
          f"{sum(p[0] == 'CLEARANCE' for p in problems)} under {clear_mm} mm")


def layout_checks(path):
    """The board-specific rules this adapter has tripped before."""
    d = json.load(open(path))
    shapes = d["shape"]
    outline = [s for s in shapes if s.startswith("TRACK~") and s.split("~")[2] == "10"]
    v = [float(x) for x in outline[0].split("~")[4].split()]
    X0, X1, Y0, Y1 = min(v[0::2]), max(v[0::2]), min(v[1::2]), max(v[1::2])
    mm = lambda x, y: f"({(x - X0) * U:.1f}, {(y - Y0) * U:.1f}) mm"
    issues = []

    vias = [s.split("~") for s in shapes if s.startswith("VIA~")]
    seen = collections.Counter((round(float(t[1]), 2), round(float(t[2]), 2)) for t in vias)
    issues += [f"duplicate via at {mm(*k)}" for k, n in seen.items() if n > 1]
    for t in vias:
        x, y, r = float(t[1]), float(t[2]), float(t[3]) / 2
        gap = (min(x - X0, X1 - x, y - Y0, Y1 - y) - r) * U
        if gap < 0.4:
            issues.append(f"via {t[4]} {gap:.2f} mm from the board edge at {mm(x, y)} (want >= 0.4)")

    pads = {}
    for s in shapes:
        if not s.startswith("LIB"):
            continue
        sub = s.split("#@$")
        ref = [p.split("~")[10] for p in sub if p.startswith("TEXT~P")][0]
        for p in sub:
            q = p.split("~")
            if q[0] in ("CIRCLE", "TRACK", "SOLIDREGION", "ARC", "RECT") and re.search(r"~2[1-9]~", p):
                issues.append(f"{ref}: object on an inner layer ({q[0]}) - makes an InnerLayer gerber")
            if q[0] != "PAD":
                continue
            x, y, w, h = map(float, q[2:6])
            pads[f"{ref}-{q[8]}"] = (x, y, q[7])
            if q[6] != "1" or (ref.startswith("U4") and q[8] == "6"):
                continue  # through-hole, or the LDO tab where vias belong
            pv = [float(z) for z in q[10].split()] if q[10].strip() else []
            xs = pv[0::2] or [x - w / 2, x + w / 2]
            ys = pv[1::2] or [y - h / 2, y + h / 2]
            for t in vias:
                vx, vy = float(t[1]), float(t[2])
                if min(xs) < vx < max(xs) and min(ys) < vy < max(ys):
                    issues.append(f"via inside pad {ref}-{q[8]} ({t[4]})")

    for s in shapes:
        t = s.split("~")
        if t[0] != "COPPERAREA":
            continue
        fill = json.loads([x for x in t if x.startswith("[[")][0])
        pts = [float(z) for isl in fill for r in isl for z in re.findall(r"-?\d+\.?\d*", r)]
        xs, ys = pts[0::2], pts[1::2]
        gaps = {"top": min(ys) - Y0, "right": X1 - max(xs), "bottom": Y1 - max(ys)}
        for side, g in gaps.items():
            if g * U < 0.3:
                issues.append(f"pour L{t[2]} {g * U:.2f} mm from the {side} edge (want >= 0.3, ideally 0.4)")
        if (min(xs) - X0) * U > 1.5:
            issues.append(f"pour L{t[2]} stops {(min(xs) - X0) * U:.1f} mm from the left edge; "
                          "the module's GND pad column there has no pour (keepout ends ~0.66 mm)")

    gnd_left = [t for t in vias if t[4] == "GND" and (float(t[1]) - X0) * U < 6]
    if not gnd_left:
        issues.append("no GND vias within 6 mm of the left edge (module GND pads 36-48 unstitched)")

    if "C1-2" in pads and "U1-3" in pads:
        a, b = pads["C1-2"], pads["U1-3"]
        dmm = math.hypot(a[0] - b[0], a[1] - b[1]) * U
        if dmm > 5:
            issues.append(f"C1 (bulk) is {dmm:.1f} mm from U1's 3V3 pin; put it within ~5 mm")

    refs = {k.split("-")[0] for k in pads}
    for r in ("RST", "BOOT"):
        if r in refs:
            issues.append(f"designator {r}: the BOM calls it SW1/SW2, rename it back")

    print("\nlayout checks:")
    for i in issues:
        print("  FIX", i)
    print(f"  {len(issues)} issue(s)" if issues else "  all clear")


def newest_export():
    """The most recent EasyEDA PCB export in ~/Downloads."""
    from pathlib import Path
    found = sorted(Path.home().joinpath("Downloads").glob("PCB_PCB_*.json"),
                   key=lambda p: p.stat().st_mtime)
    if not found:
        raise SystemExit("no PCB_PCB_*.json in ~/Downloads; pass the file as an argument")
    return str(found[-1])


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else newest_export()
    print("checking", path)
    main(path, float(sys.argv[2]) if len(sys.argv) > 2 else 0.127)
    layout_checks(path)
