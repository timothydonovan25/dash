"""Build the EasyEDA Std schematic and netlist for the K-line adapter.

Symbols come from the JLCEDA/EasyEDA Official Library (https://lceda.cn/,
https://easyeda.com), fetched per LCSC part number into a local cache.

Each functional block is drawn with real wires. Power pins get GND / VCC /
VIN power symbols. Net labels are used only where a net leaves its block,
mostly on the ESP32-C6 module's dense pin columns. Every wire is a single
straight segment that meets others only at its ends, with a junction dot
wherever three or more meet, so EasyEDA's connectivity is unambiguous.

Before writing anything, the script rebuilds the netlist from the geometry
it drew (pin ends, wire ends, flags, labels) and refuses to write if that
differs from NETS.

    python3 -I gen_sch.py <symbol cache dir> <out.json> <out.net>
"""
import json
import re
import sys
import time
from collections import defaultdict
from pathlib import Path

# designator: (LCSC part, value shown on the sheet)
PARTS = {
    "C3":   ("C1591",     "100nF"),
    "C4":   ("C13585",    "10uF 50V"),
    "C5":   ("C1591",     "100nF"),
    "C6":   ("C1591",     "100nF"),
    "C8":   ("C19702",    "10uF"),
    "C9":   ("C5673",     "1uF"),
    "C10":  ("C45783",    "22uF"),
    "D1":   ("C2827654",  "USBLC6-2SC6"),
    "D2":   ("C52023881", "SS34"),
    "D3":   ("C81598",    "1N4148W DNP"),
    "D4":   ("C148222",   "SMAJ24A"),
    "Q1":   ("C8545",     "2N7002"),
    "R1":   ("C98220",    "10k"),
    "R2":   ("C98220",    "10k"),
    "R4":   ("C98220",    "10k"),
    "R7":   ("C22775",    "100"),
    "R8":   ("C25803",    "100k"),
    "R9":   ("C21190",    "1k DNP"),
    "R10":  ("C98220",    "10k"),
    "SW1":  ("C139797",   "RST"),
    "SW2":  ("C139797",   "BOOT"),
    "U1":   ("C6553337",  "ESP32-C6-MINI-1-H4"),
    "U3":   ("C190544",   "TLIN1029DRQ1"),
    "U4":   ("C2877359",  "TPS7B8233QKVURQ1"),
    "U6":   ("C918122",   "DB125-2.54-4P"),
    "USB1": ("C404969",   "MicroXNJ"),
}

# The connections, by pin number as the LCSC symbols and footprints number
# them. U4 is the TO-252 TPS7B82: 1 IN, 2 EN, 4 DNC, 5 OUT, 6 GND (tab).
U1_GND = ["U1-1", "U1-2", "U1-11", "U1-14"] + [f"U1-{n}" for n in range(36, 54)]
NETS = {
    "VIN":       ["USB1-1", "D2-1", "D4-1", "C3-2", "C4-2", "U3-7", "D3-2", "U4-1", "U4-2"],
    "VCC":       ["U4-5", "C6-2", "C8-2", "C10-2", "R1-1", "R2-1", "R4-1", "R10-2",
                  "C5-1", "D1-5", "U1-3"],
    "GND":       ["C3-1", "C4-1", "C5-2", "C6-1", "C8-1", "C9-2", "C10-1", "D1-2", "D4-2",
                  "Q1-2", "R8-2", "SW1-4", "SW2-2", "U3-5", "U4-6", "U6-1",
                  "USB1-5", "USB1-6", "USB1-7", "USB1-8", "USB1-9"] + U1_GND,
    "BAT_IN":    ["D2-2", "U6-3"],
    "I-":        ["USB1-2", "D1-1"],
    "I+":        ["USB1-3", "D1-3"],
    "D-":        ["D1-6", "U1-17"],
    "D+":        ["D1-4", "U1-18"],
    "EN":        ["U1-8", "R4-2", "C9-1", "SW1-2"],
    "IO8":       ["U1-22", "R1-2"],
    "BOOT":      ["U1-23", "R2-2", "SW2-4"],
    "KL_TX":     ["U3-4", "U1-26"],
    "KL_RX":     ["U3-1", "U1-6", "R10-1"],
    "KL_TRX_EN": ["U3-2", "U1-13"],
    "KL_WAKE":   ["R7-1", "U1-28"],
    "Q1_G":      ["R7-2", "Q1-1", "R8-1"],
    "KL_PU":     ["D3-1", "R9-1"],
    "KL_SERIAL": ["U3-6", "U6-4", "Q1-3", "R9-2"],
}
POWER = {"GND", "VCC", "VIN"}

# Symbol origins on the sheet, EasyEDA units (10 mil). USB1 is mirrored so
# its pins face the ESD part.
PLACE = {
    # power: connector, reverse diode, VIN rail with its shunts, regulator, VCC rail
    "U6": (720, 95), "D2": (660, 100), "D4": (580, 130), "C4": (500, 130),
    "C3": (420, 130), "U4": (300, 150), "C8": (210, 200), "C10": (130, 200),
    "C6": (50, 200),
    # K-line
    "U3": (560, 260), "R10": (480, 200), "D3": (600, 190), "R9": (600, 220),
    "Q1": (680, 330), "R7": (620, 330), "R8": (670, 390),
    # USB
    "USB1": (100, 400), "D1": (260, 380),
    # MCU and its straps
    "U1": (900, 380), "R4": (1140, 260), "C9": (1230, 340), "SW1": (1300, 330),
    "R1": (1100, 420), "R2": (1180, 570), "SW2": (1220, 520), "C5": (1100, 640),
}
MIRROR = {"USB1"}


# --- symbols -------------------------------------------------------------

def fetch_symbols(lib_dir: Path) -> None:
    import urllib.request
    lib_dir.mkdir(parents=True, exist_ok=True)
    for lcsc, _ in sorted(set(PARTS.values())):
        f = lib_dir / f"{lcsc}.json"
        if f.exists() and f.read_text().lstrip().startswith("{"):
            continue
        # Same library, same uuids; easyeda.com rate-limits, lceda.cn is the fallback.
        for host in ("easyeda.com", "lceda.cn"):
            url = f"https://{host}/api/products/{lcsc}/components?version=6.5.54"
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            try:
                f.write_bytes(urllib.request.urlopen(req, timeout=30).read())
                break
            except OSError:
                continue
        time.sleep(1.5)


NUM = r"-?\d+(?:\.\d+)?"


def fmt(v: float) -> str:
    return str(int(v)) if float(v).is_integer() else f"{v:.4f}".rstrip("0").rstrip(".")


def move_path(d: str, dx: float, dy: float) -> str:
    """Translate the absolute commands of an SVG path; relative ones stay."""
    out, cmd, args = [], None, []
    arity = {"M": 2, "L": 2, "T": 2, "C": 6, "S": 4, "Q": 4, "A": 7, "H": 1, "V": 1}

    def flush():
        if cmd is None:
            return
        n = arity.get(cmd, 0)
        vals = [float(a) for a in args]
        if n:
            for i in range(0, len(vals), n):
                g = vals[i:i + n]
                if cmd == "A":
                    g[5] += dx; g[6] += dy
                elif cmd == "H":
                    g[0] += dx
                elif cmd == "V":
                    g[0] += dy
                else:
                    for j in range(0, len(g), 2):
                        g[j] += dx; g[j + 1] += dy
                vals[i:i + n] = g
        out.append(cmd + " " + " ".join(fmt(v) for v in vals))

    for t in re.findall(r"[A-Za-z]|" + NUM, d):
        if t.isalpha():
            flush(); cmd, args = t, []
        else:
            args.append(t)
    flush()
    return " ".join(out)


def move_pts(s: str, dx: float, dy: float) -> str:
    v = [float(x) for x in s.split()]
    return " ".join(fmt(v[i] + (dx if i % 2 == 0 else dy)) for i in range(len(v)))


def move_shape(s: str, dx, dy):
    f = s.split("~")
    t = f[0]
    add = lambda i, d: fmt(float(f[i]) + d)
    if t in ("PL", "PG"):
        f[1] = move_pts(f[1], dx, dy)
    elif t in ("R", "E"):
        f[1], f[2] = add(1, dx), add(2, dy)
    elif t in ("PT", "A"):
        f[1] = move_path(f[1], dx, dy)
    elif t == "T":
        f[2], f[3] = add(2, dx), add(3, dy)
    elif t == "P":
        seg = s.split("^^")
        m = seg[0].split("~")
        m[4], m[5] = fmt(float(m[4]) + dx), fmt(float(m[5]) + dy)
        seg[0] = "~".join(m)
        seg[1] = move_pts(seg[1].replace("~", " "), dx, dy).replace(" ", "~")
        p = seg[2].split("~"); p[0] = move_path(p[0], dx, dy); seg[2] = "~".join(p)
        for k in (3, 4):
            q = seg[k].split("~")
            q[1], q[2] = fmt(float(q[1]) + dx), fmt(float(q[2]) + dy)
            seg[k] = "~".join(q)
        if len(seg) > 5 and seg[5]:
            q = seg[5].split("~"); q[1], q[2] = fmt(float(q[1]) + dx), fmt(float(q[2]) + dy)
            seg[5] = "~".join(q)
        if len(seg) > 6 and seg[6]:
            q = seg[6].split("~"); q[1] = move_path(q[1], dx, dy); seg[6] = "~".join(q)
        return "^^".join(seg)
    else:
        raise ValueError(f"shape type {t} not handled")
    return "~".join(f)


def mirror_shape(s: str, cx: float):
    """Mirror left-right about x = cx. Only what MIRROR's symbols contain."""
    mx = lambda v: fmt(2 * cx - float(v))
    swap = {"start": "end", "end": "start"}
    f = s.split("~")
    if f[0] == "R":
        f[1] = fmt(2 * cx - float(f[1]) - float(f[5]))
        return "~".join(f)
    if f[0] != "P":
        raise ValueError(f"cannot mirror shape type {f[0]}")
    seg = s.split("^^")
    m = seg[0].split("~")
    m[4] = mx(m[4])
    m[6] = {"0": "180", "180": "0"}.get(m[6], m[6])
    seg[0] = "~".join(m)
    d = seg[1].split("~"); d[0] = mx(d[0]); seg[1] = "~".join(d)
    p = seg[2].split("~")
    g = re.match(r"M\s*(" + NUM + r")[,\s]\s*(" + NUM + r")\s*([hv])\s*(" + NUM + ")$", p[0].strip())
    x, y, hv, ln = g.groups()
    ln = fmt(-float(ln)) if hv == "h" else ln
    p[0] = f"M {mx(x)} {y} {hv} {ln}"
    seg[2] = "~".join(p)
    for k in (3, 4):
        q = seg[k].split("~")
        q[1] = mx(q[1]); q[5] = swap.get(q[5], q[5])
        seg[k] = "~".join(q)
    if len(seg) > 5 and seg[5]:
        q = seg[5].split("~"); q[1] = mx(q[1]); seg[5] = "~".join(q)
    if len(seg) > 6 and seg[6]:
        q = seg[6].split("~")
        q[1] = re.sub(r"([ML])\s*(" + NUM + r")\s+(" + NUM + ")",
                      lambda g: f"{g[1]} {mx(g[2])} {g[3]}", q[1])
        seg[6] = "~".join(q)
    return "^^".join(seg)


def pins_of(shapes):
    """{number: (name, x, y)} at each pin's connection end."""
    out = {}
    for s in shapes:
        if s.startswith("P~"):
            seg = s.split("^^")
            m = seg[0].split("~")
            out[m[3]] = (seg[3].split("~")[4], float(m[4]), float(m[5]))
    return out


# --- drawing -------------------------------------------------------------

class Sheet:
    def __init__(self):
        n = iter(range(0x1000, 0x100000))
        self.gid = lambda: f"gge{next(n):06x}"
        self.parts = {}      # ref -> (head, body, pins)
        self.segs = []       # ((x1, y1), (x2, y2))
        self.flags = []      # (net, x, y, dir)
        self.labels = []     # (net, x, y, anchor)

    def place(self, ref, lcsc, value, ds):
        h = ds["head"]
        X, Y = PLACE[ref]
        dx, dy = X - float(h["x"]), Y - float(h["y"])
        body = [move_shape(s, dx, dy) for s in ds["shape"]]
        if ref in MIRROR:
            body = [mirror_shape(s, X) for s in body]
        body = [re.sub(r"(?<=~)gge[0-9a-f]+(?=~|\^|$)", lambda _: self.gid(), s) for s in body]
        pins = pins_of(body)
        xs = [p[1] for p in pins.values()]
        ys = [p[2] for p in pins.values()]
        tx, ty = min(xs), min(ys) - 14
        body.append(f"T~P~{fmt(tx)}~{fmt(ty)}~0~#000080~Arial~~~~~comment~{ref}~1~start~{self.gid()}~0")
        body.append(f"T~N~{fmt(tx)}~{fmt(ty + 8)}~0~#000080~Arial~~~~~comment~{value}~1~start~{self.gid()}~0")
        cp = dict(h["c_para"]); cp.pop("pre", None)
        cp["nameAlias"] = "Value"; cp["Value"] = value
        if "DNP" in value:
            cp["Value"] = value.replace(" DNP", "")
        attrs = "`".join(f"{k}`{v}" for k, v in cp.items()) + "`"
        bom = "no" if "DNP" in value else "yes"
        # x, y, attributes, rotation, import flag, id, footprint uuid, symbol
        # uuid, locked, (deprecated), update to PCB, add to BOM.
        head = (f"LIB~{fmt(X)}~{fmt(Y)}~{attrs}~~0~{self.gid()}~{h['puuid']}~{h['uuid']}"
                f"~0~~yes~{bom}")
        self.parts[ref] = (head, body, pins)

    def pin(self, spec):
        ref, num = spec.split("-")
        _, x, y = self.parts[ref][2][num]
        return (x, y)

    def wire(self, *pts):
        """A polyline through points; a str point is a pin ('U3-6'), a tuple is
        absolute, and ('+', dx, dy) is relative to the previous point."""
        abs_pts = []
        for p in pts:
            if isinstance(p, str):
                p = self.pin(p)
            elif p[0] == "+":
                p = (abs_pts[-1][0] + p[1], abs_pts[-1][1] + p[2])
            abs_pts.append(p)
        for a, b in zip(abs_pts, abs_pts[1:]):
            assert a[0] == b[0] or a[1] == b[1], f"diagonal wire {a}->{b}"
            if a != b:
                self.segs.append((a, b))
        return abs_pts[-1]

    def flag(self, net, at, d):
        self.flags.append((net, at[0], at[1], d))

    def label(self, net, at, anchor):
        self.labels.append((net, at[0], at[1], anchor))

    def power(self, spec, net, d, *path):
        """Stub from a pin along path (relative moves), then a power symbol."""
        pts = [spec] + [("+",) + m for m in path]
        end = self.wire(*pts)
        self.flag(net, end, d)

    def tag(self, spec, net, length, side):
        """Stub from a pin, then a net label at its end. side 'L' or 'R'."""
        sign = -1 if side == "L" else 1
        end = self.wire(spec, ("+", sign * length, 0))
        self.label(net, end, "end" if side == "L" else "start")


def draw(sh: Sheet):
    w, p, t = sh.wire, sh.power, sh.tag

    # Power in: DLC 12 V -> SS34 -> VIN rail -> TVS and bulk -> LDO -> VCC rail
    w("D2-2", "U6-3")
    p("U6-1", "GND", "L", (-10, 0))
    w("D2-1", (250, 100), (250, 135), "U4-1")
    w((250, 135), (250, 145), "U4-2")
    sh.flag("VIN", (250, 100), "U")
    w("D4-1", ("+", -10, 0), ("+", 0, -30))
    w("C4-2", ("+", 10, 0), ("+", 0, -30))
    w("C3-2", ("+", 10, 0), ("+", 0, -30))
    p("D4-2", "GND", "D", (10, 0))
    p("C4-1", "GND", "D", (-10, 0))
    p("C3-1", "GND", "D", (-10, 0))
    p("U4-6", "GND", "D", (10, 0))
    w("U4-5", (80, 165))
    sh.flag("VCC", (80, 165), "U")
    for c in ("C8", "C10", "C6"):
        w(f"{c}-2", ("+", 10, 0), ("+", 0, -35))
        p(f"{c}-1", "GND", "D", (-10, 0))

    # K-line: transceiver, wake-up FET, optional pull-up, to the connector
    w("U3-6", (690, 260), (690, 110), "U6-4")
    w("Q1-3", (690, 260))
    p("U3-7", "VIN", "U", (20, 0))
    p("U3-5", "GND", "D", (10, 0))
    w("U3-1", (450, 240))
    w("R10-1", ("+", -10, 0), (450, 240))
    w((450, 240), (430, 240))
    sh.label("KL_RX", (430, 240), "end")
    p("R10-2", "VCC", "U", (10, 0))
    t("U3-2", "KL_TRX_EN", 40, "L")
    t("U3-4", "KL_TX", 40, "L")
    w("D3-1", ("+", -10, 0), ("+", 0, 30), "R9-1")
    p("D3-2", "VIN", "U", (10, 0))
    w("R9-2", (690, 220))
    w("R7-2", "Q1-1")
    w("R8-1", ("+", -10, 0), "R7-2")
    t("R7-1", "KL_WAKE", 10, "L")
    p("Q1-2", "GND", "D", (0, 10))
    p("R8-2", "GND", "D", (10, 0))

    # USB: connector, ESD, then to the module by label
    w("USB1-5", "D1-2")
    w("USB1-3", ("+", 30, 0), ("+", 0, -10), "D1-3")
    w("USB1-2", ("+", 60, 0), ("+", 0, -40), "D1-1")
    p("USB1-1", "VIN", "R", (10, 0))
    w("USB1-7", ("+", 0, -10), ("+", 70, 0), ("+", 0, 40))
    w("USB1-6", ("+", 0, -10))
    w((150, 340), (170, 340))
    sh.flag("GND", (170, 340), "D")
    w("USB1-8", ("+", 0, 10), ("+", 10, 0))
    w("USB1-9", ("+", 0, 10))
    sh.flag("GND", (90, 460), "D")
    t("D1-6", "D-", 50, "R")
    t("D1-4", "D+", 50, "R")
    p("D1-5", "VCC", "R", (10, 0))

    # The module: labels on the left column, a GND bus on the right
    w("U1-1", ("+", -10, 0), ("+", 0, 10))
    w("U1-2", ("+", -10, 0))
    w((845, 250), (835, 250))
    sh.flag("GND", (835, 250), "L")
    p("U1-3", "VCC", "L", (-10, 0))
    p("U1-11", "GND", "D", (-15, 0))
    p("U1-14", "GND", "D", (-15, 0))
    for spec, net in (("U1-6", "KL_RX"), ("U1-8", "EN"), ("U1-13", "KL_TRX_EN"),
                      ("U1-17", "D-"), ("U1-18", "D+"), ("U1-22", "IO8"),
                      ("U1-23", "BOOT"), ("U1-26", "KL_TX")):
        t(spec, net, 20, "L")
    gnd = [f"U1-{n}" for n in range(53, 35, -1)]
    prev = None
    for spec in gnd:
        end = w(spec, ("+", 10, 0))
        if prev:
            w(prev, end)
        prev = end
    w(prev, ("+", 10, 0))
    sh.flag("GND", (prev[0] + 10, prev[1]), "D")
    t("U1-28", "KL_WAKE", 20, "R")

    # EN: pull-up, delay cap, reset button
    w((1060, 300), (1340, 300), ("+", 0, 25), "SW1-2")
    sh.label("EN", (1060, 300), "end")
    w("R4-2", ("+", 10, 0), (1170, 300))
    p("R4-1", "VCC", "U", (-10, 0))
    w("C9-1", ("+", -10, 0), (1205, 300))
    p("C9-2", "GND", "D", (10, 0))
    p("SW1-4", "GND", "D", (20, 0))

    # IO8 strap, BOOT strap and button, module decoupling
    p("R1-1", "VCC", "U", (-10, 0))
    t("R1-2", "IO8", 30, "R")
    w("SW2-4", (1270, 535), (1270, 570), "R2-2")
    w((1270, 570), (1290, 570))
    sh.label("BOOT", (1290, 570), "start")
    p("SW2-2", "GND", "R", (10, 0))
    p("R2-1", "VCC", "U", (-10, 0))
    p("C5-1", "VCC", "U", (-10, 0))
    p("C5-2", "GND", "D", (10, 0))


# --- connectivity --------------------------------------------------------

def split_segments(segs, points):
    """Split each segment wherever a point of interest lies inside it, so that
    every connection on the sheet is at a segment end."""
    out = []
    for a, b in segs:
        cuts = [a, b]
        for q in points:
            if q in (a, b):
                continue
            if a[0] == b[0] == q[0] and min(a[1], b[1]) < q[1] < max(a[1], b[1]):
                cuts.append(q)
            elif a[1] == b[1] == q[1] and min(a[0], b[0]) < q[0] < max(a[0], b[0]):
                cuts.append(q)
        cuts = sorted(set(cuts), key=lambda q: (q[0] - a[0]) ** 2 + (q[1] - a[1]) ** 2)
        out += list(zip(cuts, cuts[1:]))
    return out


def check(sh: Sheet):
    """Rebuild the netlist from the drawing and compare it with NETS."""
    pin_at = defaultdict(list)
    for ref, (_, _, pins) in sh.parts.items():
        for num, (_, x, y) in pins.items():
            pin_at[(x, y)].append(f"{ref}-{num}")
    for q, ps in pin_at.items():
        assert len(ps) == 1, f"pins on top of each other: {ps}"
    ends = {q for s in sh.segs for q in s}
    pois = ends | set(pin_at) | {(x, y) for _, x, y, _ in sh.flags + sh.labels}
    sh.segs = split_segments(sh.segs, pois)

    # Overlapping collinear segments would join nets silently.
    seen = set()
    for a, b in sh.segs:
        key = tuple(sorted((a, b)))
        assert key not in seen, f"duplicate wire {key}"
        seen.add(key)

    parent = {}
    find = lambda q: q if parent.setdefault(q, q) == q else find(parent[q])
    def union(a, b):
        parent[find(a)] = find(b)
    for a, b in sh.segs:
        union(a, b)
    names = defaultdict(set)
    for net, x, y, _ in sh.flags + sh.labels:
        names[find((x, y))].add(net)
    got = defaultdict(set)
    for q, ps in pin_at.items():
        n = names.get(find(q), set())
        assert len(n) <= 1, f"{ps} joins nets {n}"
        if n:
            got[next(iter(n))].update(ps)
        else:
            group = {r2 for r in pin_at if find(r) == find(q) for r2 in pin_at[r]}
            if len(group) > 1:
                # A net drawn only with wires: name it by the NETS entry it equals.
                match = [k for k, v in NETS.items() if set(v) == group]
                got[match[0] if match else "?" + ps[0]].update(group)
    for net, x, y, _ in sh.flags + sh.labels:
        assert find((x, y)) in {find(q) for q in pin_at}, f"{net} at {(x, y)} touches no pin"
    want = {n: set(v) for n, v in NETS.items()}
    bad = {n: sorted(want.get(n, set()) ^ got.get(n, set()))
           for n in set(want) | set(got) if want.get(n) != got.get(n)}
    if bad:
        raise SystemExit(f"drawing does not match NETS: {bad}")

    deg = defaultdict(int)
    for a, b in sh.segs:
        deg[a] += 1; deg[b] += 1
    for q in pin_at:
        if q in deg:
            deg[q] += 1
    return [q for q, n in deg.items() if n >= 3]


# --- output --------------------------------------------------------------

GND_GLYPH = [((0, 0), (0, 10)), ((-10, 10), (10, 10)), ((-6, 12), (6, 12)),
             ((-3, 14), (3, 14)), ((-1, 16), (1, 16))]
BAR_GLYPH = [((0, 0), (0, 10)), ((-6, 10), (6, 10))]
ROT = {"D": lambda u, v: (u, v), "U": lambda u, v: (u, -v),
       "L": lambda u, v: (-v, u), "R": lambda u, v: (v, u)}


def flag_str(sh, net, x, y, d):
    glyph = GND_GLYPH if net == "GND" else BAR_GLYPH
    kind = "part_netLabel_gnD" if net == "GND" else "part_netLabel_VCC"
    angle = {"D": 0, "R": 90, "U": 180, "L": 270}[d] if net == "GND" else \
            {"U": 0, "L": 90, "D": 180, "R": 270}[d]
    r = ROT[d]
    pls = []
    for (u1, v1), (u2, v2) in glyph:
        a, b = r(u1, v1), r(u2, v2)
        pls.append(f"PL~{fmt(x + a[0])} {fmt(y + a[1])} {fmt(x + b[0])} {fmt(y + b[1])}"
                   f"~#000000~1~0~none~{sh.gid()}~0")
    tu, tv = r(0, 20)
    anchor = {"U": "middle", "D": "middle", "L": "end", "R": "start"}[d]
    vis = 0 if net == "GND" else 1
    mark = (f"{net}~#000080~{fmt(x + tu)}~{fmt(y + tv + (3 if d in 'LR' else 0))}~0~{anchor}~{vis}"
            f"~Times New Roman~9pt~{sh.gid()}")
    return "^^".join([f"F~{kind}~{fmt(x)}~{fmt(y)}~{angle}~{sh.gid()}~~0",
                      f"{fmt(x)}~{fmt(y)}", mark] + pls)


def netlist(sh: Sheet) -> str:
    out = []
    for ref in sorted(sh.parts, key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r)))):
        lcsc, value = PARTS[ref]
        fp = re.search(r"package`([^`]*)", sh.parts[ref][0]).group(1)
        out += ["[", ref, fp, value, "", "", "", "]"]
    out.append("")
    for net, pins in NETS.items():
        out += ["(", net + " "] + pins + [")", ""]
    return "\n".join(out) + "\n"


def main(lib_dir: Path, out_json: Path, out_net: Path) -> None:
    fetch_symbols(lib_dir)
    sh = Sheet()
    for ref, (lcsc, value) in PARTS.items():
        ds = json.loads((lib_dir / f"{lcsc}.json").read_text())["result"]["dataStr"]
        sh.place(ref, lcsc, value, ds)
    draw(sh)
    junctions = check(sh)

    shapes = [head + "".join("#@$" + s for s in body) for head, body, _ in sh.parts.values()]
    shapes += [f"W~{fmt(a[0])} {fmt(a[1])} {fmt(b[0])} {fmt(b[1])}~#008800~1~0~none~{sh.gid()}~0"
               for a, b in sh.segs]
    shapes += [f"J~{fmt(x)}~{fmt(y)}~2.5~#CC0000~{sh.gid()}~0" for x, y in junctions]
    shapes += [flag_str(sh, *f) for f in sh.flags]
    for net, x, y, anchor in sh.labels:
        tx = x - 2 if anchor == "end" else x + 2
        shapes.append(f"N~{fmt(x)}~{fmt(y)}~0~#0000ff~{net}~{sh.gid()}~{anchor}~{fmt(tx)}~"
                      f"{fmt(y - 2)}~Times New Roman~7pt~0")
    shapes.append("T~L~20~700~0~#000000~Times New Roman~9pt~~~~comment~Symbols: JLCEDA/EasyEDA "
                  f"Official Library, https://lceda.cn/ https://easyeda.com~1~start~{sh.gid()}~0~")
    doc = {
        "head": {"docType": "1", "editorVersion": "6.5.54", "newgId": True,
                 "c_para": {"Prefix Start": "1"}, "c_spiceCmd": None,
                 "hasIdFlag": True, "x": "0", "y": "0"},
        "canvas": "CA~1500~800~#FFFFFF~yes~#CCCCCC~5~1500~800~line~5~pixel~5~0~0",
        "shape": shapes,
        "BBox": {"x": 0, "y": 0, "width": 1500, "height": 800},
        "colors": {},
    }
    out_json.write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")
    out_net.write_text(netlist(sh), encoding="utf-8")
    print(f"{len(sh.parts)} parts, {len(sh.segs)} wires, {len(junctions)} junctions, "
          f"{len(sh.flags)} power symbols, {len(sh.labels)} labels")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
