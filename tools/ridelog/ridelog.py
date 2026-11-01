#!/usr/bin/env python3
"""What a ride says about the assumptions the firmware was built on.

    ridelog.py CARD [--session N] [--plots DIR]

CARD is the SD card's root (with logs/ and detail/ in it), or a directory
holding one session's files. With no --session, the newest on the card.

Reads the blackbox (logs/<stem>.csv) and the detail log beside it
(detail/<stem>_{imu,tach,det,evt,diag}.csv; core/log/detail.hpp) and prints a
report, one section per assumption, most suspect first: each says what was
assumed, what the ride measured, and a verdict where a number can give one.
A verdict is a pointer to what to look at, not a proof: read the numbers.

Standard library only. With numpy, the IMU section adds a vibration
spectrum; with matplotlib and --plots, PNGs of the main traces.
"""
import argparse
import csv
import math
import os
import re
import shlex
import sys
from collections import Counter, defaultdict

try:
    import numpy as np
except ImportError:  # the report still runs, without the spectrum
    np = None

STREAMS = ("imu", "tach", "det", "evt", "diag")
SESSION_RE = re.compile(r"^(?:(\d{4}-\d{2}-\d{2})_)?s(\d{3})(?:_([a-z]+))?\.csv$")

# Budgets and expectations from the firmware, kept here so a verdict names
# the number it was judged against.
LAMP_BUDGET_US = 3000          # tach edge to shift lamp (CLAUDE.md)
IMU_HZ, TACH_HZ, DET_HZ = 200, 50, 50
SLOW_SD_MS = 100


# --- reading ---------------------------------------------------------------------

class Table:
    """A CSV with '#' preamble lines, as columns. Numbers become floats where
    every value in the column is one; empty fields are None."""

    def __init__(self, path):
        self.path = path
        self.preamble = []
        self.cols = {}
        self.names = []
        self.lap_tags = []
        rows = []
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            header = None
            for line in f:
                if line.startswith("#"):
                    if header is None:
                        self.preamble.append(line[1:].strip())
                    else:
                        self.lap_tags.append(line[1:].strip())
                    continue
                if header is None:
                    header = next(csv.reader([line]))
                    continue
                if line.strip():
                    rows.append(line)
        self.names = header or []
        data = list(csv.reader(rows))
        # A row cut short by a power cut is the last one; drop it.
        if data and len(data[-1]) != len(self.names):
            data = data[:-1]
        self.n = len(data)
        for i, name in enumerate(self.names):
            raw = [r[i] if i < len(r) else "" for r in data]
            self.cols[name] = _numeric(raw)

    def __getitem__(self, name):
        return self.cols.get(name, [])

    def has(self, name):
        return name in self.cols


def _numeric(raw):
    out = []
    for v in raw:
        if v == "":
            out.append(None)
            continue
        try:
            out.append(float(v))
        except ValueError:
            return [x if x != "" else None for x in raw]
    return out


def find_session(card, want):
    """{stream: path} for one session; '' is the blackbox."""
    dirs = [d for d in (os.path.join(card, "logs"), os.path.join(card, "detail"), card)
            if os.path.isdir(d)]
    found = defaultdict(dict)
    for d in dirs:
        for name in os.listdir(d):
            m = SESSION_RE.match(name)
            if not m:
                continue
            found[int(m.group(2))][m.group(3) or ""] = os.path.join(d, name)
    if not found:
        sys.exit(f"ridelog: no session files under {card}")
    session = want if want is not None else max(found)
    if session not in found:
        sys.exit(f"ridelog: session {session} not found; the card has "
                 + ", ".join(str(s) for s in sorted(found)))
    return session, found[session]


# --- small statistics ----------------------------------------------------------------

def vals(col):
    return [v for v in col if isinstance(v, float)]


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    k = (len(xs) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def median(xs):
    return pct(xs, 50)


def fmt(v, d=1, unit=""):
    if v is None:
        return "--"
    return f"{v:.{d}f}{unit}"


def where(mask, col):
    return [v for m, v in zip(mask, col) if m and isinstance(v, float)]


class Report:
    def __init__(self):
        self.verdicts = []

    def section(self, title, assumption):
        print()
        print("=" * 78)
        print(title)
        print("  assumed: " + assumption)
        print("-" * 78)

    def line(self, label, value):
        print(f"  {label:<38} {value}")

    def verdict(self, ok, text):
        tag = "OK  " if ok is True else ("FAIL" if ok is False else "LOOK")
        print(f"  [{tag}] {text}")
        self.verdicts.append((tag, text))

    def note(self, text):
        print(f"         {text}")


# --- the sections ------------------------------------------------------------------

def header_info(t):
    info = {}
    for p in t.preamble:
        m = re.match(r"session (\d+), reset (.+) \((-?\d+)\)", p)
        if m:
            info["reset"] = m.group(2)
        m = re.match(r"firmware (.+)", p)
        if m:
            info["firmware"] = m.group(1)
        m = re.match(r"tach refractory (\d+) us", p)
        if m:
            info["refractory_us"] = int(m.group(1))
        m = re.match(r"setting (.+?) = (.+) \[(0x[0-9A-Fa-f]+)\]", p)
        if m:
            info.setdefault("settings", {})[m.group(1)] = m.group(2)
    return info


def parse_events(t):
    events = []
    for time, kind, detail in zip(t["Time"], t["Event"], t["Detail"]):
        kv = {}
        for tok in shlex.split(detail or ""):
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        events.append((time, kind, kv))
    return events


def section_files(r, files, tables, diag):
    r.section("0. The log itself",
              "the card keeps up: no ring drops, no long stalls, every stream at its rate")
    for s, path in sorted(files.items()):
        r.line(s or "blackbox", os.path.relpath(path))
    expect = {"imu": IMU_HZ, "tach": TACH_HZ, "det": DET_HZ, "diag": 1}
    for s, hz in expect.items():
        t = tables.get(s)
        if not t or t.n < 2:
            continue
        ts = vals(t["Time"])
        dur = ts[-1] - ts[0]
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        rate = (len(ts) - 1) / dur if dur > 0 else 0
        r.line(f"{s}: rows, span, rate", f"{t.n}, {dur:.0f} s, {rate:.1f} Hz (want {hz})")
        r.line(f"{s}: largest gap", fmt(max(gaps) * 1000, 0, " ms"))
    if diag and diag.n:
        last = lambda c: (vals(diag[c]) or [0])[-1]
        drops = {s: last("Drop_" + s) for s in ("main",) + STREAMS if diag.has("Drop_" + s)}
        r.line("rows dropped (ring full)", ", ".join(f"{k} {v:.0f}" for k, v in drops.items()))
        sd_max = max(vals(diag["SdMaxMs"]) or [0])
        slow = sum(vals(diag["SdSlow"]))
        errs = sum(vals(diag["SdErrors"]))
        r.line("slowest card write/sync", fmt(sd_max, 0, " ms"))
        r.line(f"card calls over {SLOW_SD_MS} ms", f"{slow:.0f}")
        r.line("card errors", f"{errs:.0f}")
        kbs = vals(diag["SdBytes"])
        r.line("card throughput, mean", fmt(mean(kbs) / 1024 if kbs else None, 1, " kB/s"))
        r.verdict(sum(drops.values()) == 0,
                  "no rows dropped" if sum(drops.values()) == 0 else
                  "rows were dropped: a card stall outlasted a ring (10 s)")
        r.verdict(None if sd_max > 500 else True,
                  f"slowest card call {sd_max:.0f} ms (rings hold ~10 s)")


def section_power(r, info, events, diag):
    r.section("1. Power and the battery",
              "the bike's supply is clean enough: no brownout resets, VIN cuts only at key-off,"
              " the battery thresholds mean something")
    r.line("this power-on began with", info.get("reset", "--"))
    r.verdict(info.get("reset") not in ("brownout", "panic", "task watchdog",
                                         "interrupt watchdog", "other watchdog"),
              f"reset reason: {info.get('reset', 'unknown')}")
    cuts = [e for e in events if e[1] == "power_fail"]
    r.line("VIN-cut events", str(len([c for c in cuts if c[2].get("cut") == "1"])))
    for t, _, kv in cuts[:10]:
        r.note(f"{t:9.3f} s  cut={kv.get('cut')}")
    if diag and diag.n:
        vb = [v for v in vals(diag["VBat"]) if v > 0]
        if vb:
            r.line("VIN min / median / max", f"{min(vb):.2f} / {median(vb):.2f} / {max(vb):.2f} V")
        rest = [v for v in vals(diag["RestV"]) if v > 0]
        crank = [v for v in vals(diag["CrankV"]) if v > 0]
        charge = [v for v in vals(diag["ChargeV"]) if v > 0]
        r.line("battery rest / crank dip / charging",
               f"{fmt(rest[-1] if rest else None, 2)} / {fmt(crank[-1] if crank else None, 2)}"
               f" / {fmt(charge[-1] if charge else None, 2)} V")
        verdicts = [v for v in diag["Battery"] if v]
        r.line("battery verdict (last)", verdicts[-1] if verdicts else "--")
    for t, _, kv in [e for e in events if e[1] == "battery"]:
        r.note(f"{t:9.3f} s  {kv}")


def section_tach(r, info, tach, det):
    refr = info.get("refractory_us")
    r.section("2. The tach input",
              f"one clean edge per spark through R34/C19 and a {refr or '?'} us refractory window;"
              " revs per pulse as set")
    if not tach or not tach.n:
        r.note("no tach file")
        return
    running = [p > 0 for p in (v or 0 for v in tach["RPM"])]
    pulses = sum(vals(tach["TachPulses"]))
    rejected = sum(vals(tach["TachRejected"]))
    shorts = sum(vals(tach["TachShort"]))
    longs = sum(vals(tach["TachLong"]))
    torn = sum(vals(tach["TachTorn"]))
    rpm = where(running, tach["RPM"])
    r.line("pulses counted", f"{pulses:.0f}")
    r.line("edges thrown away by the window", f"{rejected:.0f}"
           + (f"  ({rejected / pulses:.2f} per pulse)" if pulses else ""))
    ring = [v for v in vals(tach["TachRingUs"]) if v > 0]
    if ring:
        r.line("ringing after the spark p50/p99/max",
               f"{pct(ring, 50):.0f} / {pct(ring, 99):.0f} / {max(ring):.0f} us")
        if refr:
            r.verdict(max(ring) < 0.8 * refr,
                      f"latest ring {max(ring):.0f} us against a {refr} us window"
                      + ("" if max(ring) < 0.8 * refr else
                         ": ringing reaches the window's end; a second count is close"))
    else:
        r.line("ringing after the spark", "none seen (no edge inside the window)")
    per_k = lambda x: 1000.0 * x / pulses if pulses else 0
    r.line("short intervals (extra edge) / 1000", f"{per_k(shorts):.2f}")
    r.line("long intervals (missed spark) / 1000", f"{per_k(longs):.2f}")
    r.line("torn reads", f"{torn:.0f}")
    r.verdict(per_k(shorts) < 1.0, f"extra edges: {per_k(shorts):.2f} per 1000 sparks")
    r.verdict(per_k(longs) < 1.0, f"missed sparks: {per_k(longs):.2f} per 1000"
              + ("" if per_k(longs) < 1.0 else " -- R34/C19 filtering the spike away?"))
    if rpm:
        r.line("rpm while running p1/p50/p99/max",
               f"{pct(rpm, 1):.0f} / {pct(rpm, 50):.0f} / {pct(rpm, 99):.0f} / {max(rpm):.0f}")
        # Idle sits low and still: if it reads near half or double a plausible
        # CRF250F idle (~1700-2000), revs-per-pulse is wrong.
        idle = [v for v, n in zip(tach["RPM"], tach["Neutral"]) if v and n == 1.0]
        if idle:
            r.line("rpm in neutral, median", f"{median(idle):.0f}")
            r.verdict(None, "compare that idle with a known idle; half means "
                            "Tach revs/pulse should be 2, double means 1")
    # The engine running while the tach shows no pulse for 3 rows in a row.
    gaps = 0
    run = 0
    for p, rp in zip(tach["TachPulses"], tach["RawRPM"]):
        if rp and rp > 0 and p == 0:
            run += 1
            if run == 5:
                gaps += 1
        else:
            run = 0
    r.line("100 ms stretches with rpm but no pulse", str(gaps))


def section_lamps(r, tach, diag):
    r.section("3. Shift-lamp latency and ctrl's budget",
              f"tach edge to lamp under {LAMP_BUDGET_US} us; ctrl (core 0, prio 18) never late")
    if not tach or not tach.n:
        return
    wake = [v for v in vals(tach["WakeLatMaxUs"]) if v > 0]
    lamp = [v for v in vals(tach["LampLatMaxUs"]) if v > 0]
    work = vals(tach["WorkMaxUs"])
    if wake:
        r.line("edge to ctrl awake p50/p99/max", f"{pct(wake, 50):.0f} / {pct(wake, 99):.0f} / {max(wake):.0f} us")
    if lamp:
        r.line("edge to lamp written p50/p99/max", f"{pct(lamp, 50):.0f} / {pct(lamp, 99):.0f} / {max(lamp):.0f} us")
        r.verdict(max(lamp) < LAMP_BUDGET_US, f"worst edge-to-lamp {max(lamp):.0f} us (budget {LAMP_BUDGET_US})")
    else:
        r.note("no edge changed the lamps (were they ever lit?)")
    if work:
        r.line("ctrl tick work p99/max", f"{pct(work, 99):.0f} / {max(work):.0f} us")
    late = vals(tach["Late"])
    if late:
        r.line("ctrl late ticks over the ride", f"{late[-1] - late[0]:.0f}")
        r.verdict(late[-1] - late[0] == 0, f"{late[-1] - late[0]:.0f} late ctrl ticks")
    errs = sum(vals(tach["LampErrors"]))
    r.line("lamp write errors", f"{errs:.0f} of {sum(vals(tach['LampWrites'])):.0f}")


def section_wheels(r, info, tach, det):
    st = info.get("settings", {})
    r.section("4. Wheel sensors and circumference",
              f"front {st.get('Wheels/Front circ.', '?')} / {st.get('Wheels/Front pulses', '?')} magnets,"
              f" rear {st.get('Wheels/Rear circ.', '?')} / {st.get('Wheels/Rear pulses', '?')};"
              " one clean pulse per magnet")
    if tach and tach.n:
        for w in ("Front", "Rear"):
            p = sum(vals(tach[w + "Pulses"]))
            s_ = sum(vals(tach[w + "Short"]))
            l_ = sum(vals(tach[w + "Long"]))
            rej = sum(vals(tach[w + "Rejected"]))
            r.line(f"{w.lower()}: pulses, short, long, rejected",
                   f"{p:.0f}, {s_:.0f}, {l_:.0f}, {rej:.0f}")
            if p:
                # Uneven magnet spacing shows as both; a lost magnet as long
                # once a revolution.
                r.verdict((s_ + l_) / p < 0.01,
                          f"{w.lower()} irregular intervals {(s_ + l_) / p * 100:.2f} %")
    if not det or not det.n:
        return
    # GPS over the wheel while steady, fast enough, good HDOP: the wheel's
    # circumference error.
    for w in ("Front", "Rear"):
        ratios = []
        for wm, gm, hd, v in zip(det[w + "MPH"], det["GpsMPH"], det["HDOP"], det["FusedValid"]):
            if wm and gm and wm > 15 and gm > 15 and hd and hd < 1.5:
                ratios.append(gm / wm)
        if len(ratios) > 50:
            m = median(ratios)
            r.line(f"GPS / {w.lower()} wheel, median ({len(ratios)} rows)", f"{m:.4f}")
            r.verdict(abs(m - 1) < 0.02,
                      f"{w.lower()} circumference is {(m - 1) * 100:+.1f} % off; "
                      f"multiply it by {m:.4f}")


def section_fusion(r, det, events):
    r.section("5. Speed fusion",
              "the gates keep the good sources and throw out spin, lockup and wheelies; bias small")
    if not det or not det.n:
        return
    moving = [(v or 0) > 5 for v in det["FusedMPH"]]
    n_mov = sum(moving)
    r.line("rows moving (>5 mph)", str(n_mov))
    if n_mov:
        for letter, name in (("A", "accel"), ("F", "front"), ("R", "rear"), ("G", "GPS")):
            used = sum(1 for m, u in zip(moving, det["Used"]) if m and u and letter in u)
            rej = sum(1 for m, u in zip(moving, det["Rejected"]) if m and u and letter in u)
            r.line(f"{name}: used / rejected while moving", f"{used / n_mov * 100:.1f} % / {rej / n_mov * 100:.1f} %")
    sig = where(moving, det["FusedSigma"])
    if sig:
        r.line("sigma p50/p99", f"{pct(sig, 50):.2f} / {pct(sig, 99):.2f} mph")
    bias = vals(det["AccelBiasG"])
    if bias:
        r.line("accel bias range", f"{min(bias):+.3f} .. {max(bias):+.3f} g")
        r.verdict(max(abs(min(bias)), abs(max(bias))) < 0.05, "accel bias stays under 0.05 g")
    resets = [e for e in events if e[1] == "fusion_reset"]
    r.line("filter resets", str(len(resets)))
    sus = [e for e in events if e[1] == "suspect"]
    r.line("suspect changes", str(len(sus)))
    for t, _, kv in sus[:8]:
        r.note(f"{t:9.3f} s  suspect={kv.get('sources')}")
    for t, _, kv in [e for e in events if e[1] == "wheel"]:
        r.note(f"{t:9.3f} s  {kv.get('wheel')} wheel -> {kv.get('verdict')}")
    shown = vals(det["ShownMPH"])
    fused = vals(det["FusedMPH"])
    if shown and fused:
        diffs = [abs(a - b) for a, b, m in zip(det["ShownMPH"], det["FusedMPH"], moving)
                 if m and a is not None and b is not None]
        if diffs:
            r.line("|speedo - fused| p99 while moving", f"{pct(diffs, 99):.2f} mph")


def imu_chip_clock(r, imu):
    """The part's own timestamp against the firmware's back-dated Time: the
    real sample rate, how far the two clocks drift apart, and how wrong the
    back-dating is, worst after a stall (Drain > 0)."""
    if not imu.has("ChipTs"):
        r.note("no ChipTs column (a log from before the chip timestamp was logged)")
        return
    ts, chip, hdr, drain = imu["Time"], imu["ChipTs"], imu["Hdr"], imu["Drain"]
    kinds = [(int(h) >> 2) & 3 for h in hdr if isinstance(h, float)]
    odr = sum(1 for k in kinds if k == 2)
    r.line("packets with an ODR timestamp (Hdr bits 3:2 = 10)",
           f"{odr} of {len(kinds)}")
    if odr < len(kinds) * 0.9:
        r.note("the FIFO is not carrying an ODR timestamp; check TMST_CONFIG and "
               "FIFO_CONFIG1's FIFO_TMST_FSYNC_EN against the datasheet")
        return
    # Segments of consecutive rows: a ring drop or a stall over ~50 ms makes
    # the 16-bit difference ambiguous (it wraps every 65.5 ms at 1 us).
    segs, cur = [], []
    for i in range(len(ts)):
        ok = all(isinstance(v, float) for v in (ts[i], chip[i]))
        if ok and cur and ts[i] - ts[cur[-1]] < 0.05:
            cur.append(i)
        else:
            if len(cur) > 1:
                segs.append(cur)
            cur = [i] if ok else []
    if len(cur) > 1:
        segs.append(cur)
    deltas = [(int(chip[b]) - int(chip[a])) % 65536
              for s in segs for a, b in zip(s, s[1:])]
    if not deltas:
        return
    raw = median(deltas)
    unit_us = 1 if raw > 2000 else 16     # TMST_RES: 1 us, or 16 us
    period_us = raw * unit_us
    r.line("chip interval, median", f"{raw:.0f} ticks = {period_us:.1f} us "
           f"(assuming {unit_us} us ticks), ODR {1e6 / period_us:.2f} Hz")
    r.verdict(abs(period_us - 5000) < 5000 * 0.03,
              f"real ODR within 3 % of 200 Hz ({(period_us / 5000 - 1) * 100:+.2f} %)")
    # Per segment: Time against the unwrapped chip clock, a straight-line
    # fit (the slope is the two crystals' drift), and what is left over is
    # the back-dating's error.
    res_all, res_backed, ppm = [], [], []
    for s in segs:
        if len(s) < 200:
            continue
        c, t = [0.0], [ts[s[0]]]
        for a, b in zip(s, s[1:]):
            c.append(c[-1] + ((int(chip[b]) - int(chip[a])) % 65536) * unit_us / 1e6)
            t.append(ts[b])
        mc, mt = mean(c), mean(t)
        var = sum((x - mc) ** 2 for x in c)
        if var <= 0:
            continue
        slope = sum((x - mc) * (y - mt) for x, y in zip(c, t)) / var
        ppm.append((slope - 1) * 1e6)
        for k, i in enumerate(s):
            e = abs(t[k] - (mt + slope * (c[k] - mc))) * 1000
            res_all.append(e)
            if drain[i]:
                res_backed.append(e)
    if not res_all:
        r.note("no run of rows long enough to fit the two clocks")
        return
    r.line("P4 clock against the chip's", f"{median(ppm):+.0f} ppm (median of {len(ppm)} runs)")
    r.line("back-dated Time error p50 / p99",
           f"{pct(res_all, 50):.2f} / {pct(res_all, 99):.2f} ms")
    if res_backed:
        r.line("... on rows from a backed-up FIFO, p50 / p99",
               f"{pct(res_backed, 50):.2f} / {pct(res_backed, 99):.2f} ms")
    r.verdict(pct(res_all, 99) < 3.0, "back-dated sample times within 3 ms (p99)")


def section_imu(r, imu, det, info):
    st = info.get("settings", {})
    r.section("6. The IMU: rate, mount, bias, vibration",
              "200 Hz without gaps on a shared SPI2; the mount offset is right; the gyro bias "
              "holds with temperature; engine vibration stays out of the numbers")
    if not imu or imu.n < 10:
        r.note("no IMU file")
        return
    ts = vals(imu["Time"])
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    big = [g for g in gaps if g > 0.0075]
    r.line("samples, mean interval", f"{imu.n}, {mean(gaps) * 1000:.2f} ms")
    r.line("gaps over 7.5 ms", f"{len(big)}, largest {max(gaps) * 1000:.0f} ms")
    backed = sum(1 for d in imu["Drain"] if d and d > 0)
    r.line("samples from a backed-up FIFO", f"{backed} ({backed / imu.n * 100:.2f} %)")
    r.verdict(len(big) < imu.n * 0.001, f"{len(big)} IMU gaps over 7.5 ms")
    imu_chip_clock(r, imu)
    for ax in ("Ax", "Ay", "Az"):
        a = vals(imu[ax])
        clip = sum(1 for v in a if abs(v) > 15.8)
        r.line(f"{ax}: min / max, near the 16 g rail", f"{min(a):+.2f} / {max(a):+.2f} g, {clip}")
    for gx in ("Gx", "Gy", "Gz"):
        g = vals(imu[gx])
        clip = sum(1 for v in g if abs(v) > 1980)
        r.line(f"{gx}: min / max, near 2000 dps", f"{min(g):+.0f} / {max(g):+.0f} dps, {clip}")
    # Still: from det's Still flag, by time.
    still_t = []
    if det and det.n:
        still_t = [t for t, s, rpm in zip(det["Time"], det["Still"], det["RPM"])
                   if s == 1.0 and (rpm or 0) == 0]
    if still_t:
        still_set = set(round(t, 1) for t in still_t)
        idx = [i for i, t in enumerate(imu["Time"]) if t is not None and round(t, 1) in still_set]
        if len(idx) > 400:
            ax = mean([imu["Ax"][i] for i in idx])
            ay = mean([imu["Ay"][i] for i in idx])
            az = mean([imu["Az"][i] for i in idx])
            roll = math.degrees(math.atan2(-ay, az))
            pitch = math.degrees(math.atan2(ax, math.hypot(ay, az)))
            r.line("gravity, parked with engine off", f"({ax:+.3f}, {ay:+.3f}, {az:+.3f}) g")
            r.line("implied mount roll / pitch", f"{roll:+.1f} / {pitch:+.1f} deg")
            r.line("set mount roll / pitch", f"{st.get('IMU/Roll offset', '?')} / {st.get('IMU/Pitch offset', '?')}")
            r.note("bars straight and the bike upright, these two should agree")
            for gx in ("Gx", "Gy", "Gz"):
                g = [imu[gx][i] for i in idx]
                sd = math.sqrt(mean([(v - mean(g)) ** 2 for v in g]))
                r.line(f"{gx} at rest: mean, noise", f"{mean(g):+.3f}, {sd:.3f} dps")
    temps = vals(imu["TempC"])
    if temps:
        r.line("IMU temperature range", f"{min(temps):.1f} .. {max(temps):.1f} C")
    if det and det.n:
        for b in ("BiasGx", "BiasGy", "BiasGz"):
            v = vals(det[b])
            if v:
                r.line(f"{b} the filter used", f"{v[-1]:+.3f} dps")
    if np is not None:
        a = np.array([v if v is not None else 0.0 for v in imu["Az"]])
        if len(a) > 2048:
            seg = a[: (len(a) // 1024) * 1024].reshape(-1, 1024)
            seg = seg - seg.mean(axis=1, keepdims=True)
            spec = (np.abs(np.fft.rfft(seg * np.hanning(1024), axis=1)) ** 2).mean(axis=0)
            freqs = np.fft.rfftfreq(1024, 1.0 / IMU_HZ)
            # Local maxima, strongest four: neighbouring bins of one peak
            # are not four peaks.
            peaks = [i for i in range(2, len(spec) - 1)
                     if spec[i] >= spec[i - 1] and spec[i] > spec[i + 1]]
            top = sorted(sorted(peaks, key=lambda i: spec[i], reverse=True)[:4])
            r.line("Az vibration peaks", ", ".join(f"{freqs[i]:.0f} Hz" for i in top))
            r.line("Az rms (all, ac)", f"{float(np.sqrt(seg.var(axis=1).mean())):.3f} g")
            r.note("peaks near rpm/60 or 2x are the engine; above 100 Hz it is folding (Nyquist)")
    else:
        r.note("(install numpy for the vibration spectrum)")


def section_lean(r, det, events):
    r.section("7. Lean, steering, slides",
              "lean from v.Omega is right on the bars; the steering estimate is small and "
              "sane; the slide gate fires on slides, not on corners")
    if not det or not det.n:
        return
    moving = [(v or 0) > 10 for v in det["FusedMPH"]]
    lean = where(moving, det["Lean"])
    if lean:
        r.line("lean left / right max", f"{min(lean):+.1f} / {max(lean):+.1f} deg")
    acc = where(moving, det["AccelLean"])
    if lean and acc:
        diff = [abs(a - b) for a, b, m in zip(det["Lean"], det["AccelLean"], moving)
                if m and a is not None and b is not None]
        r.line("|lean - accel lean| p50 moving", f"{pct(diff, 50):.1f} deg")
        r.note("the accelerometer reads ~0 in a coordinated turn; a large p50 is expected")
    steer = where(moving, det["Steer"])
    if steer:
        r.line("steering estimate p1 / p99", f"{pct(steer, 1):+.1f} / {pct(steer, 99):+.1f} deg")
        r.verdict(max(abs(pct(steer, 1)), abs(pct(steer, 99))) < 15,
                  "steering estimate within +-15 deg at speed (rake/wheelbase plausible)")
    slides = [e for e in events if e[1] == "slide" and e[2].get("on") == "1"]
    r.line("slide detections", str(len(slides)))
    for t, _, kv in slides[:10]:
        r.note(f"{t:9.3f} s  rate {kv.get('rate_dps')} dps")
    rates = [abs(v) for v in where(moving, det["SlideRate"])]
    if rates:
        r.line("|heading - course rate| p50 / p99", f"{pct(rates, 50):.1f} / {pct(rates, 99):.1f} dps")
        r.note("the gate is 20 dps (BRINGUP guess); p99 of plain riding should sit well under it")


def section_gear(r, det, events, info):
    r.section("8. Gear inference",
              "rpm/mph clusters at the CONFIG table's ratios, and the shown gear does not flap")
    if not det or not det.n:
        return
    st = info.get("settings", {})
    table = []
    for k, v in st.items():
        # "Gearing/1st" .. "Gearing/6th"; the base table is under its own labels.
        m = re.match(r"Gearing/([1-6])(st|nd|rd|th)$", k)
        if m:
            try:
                g = float(v.split()[0])
            except ValueError:
                continue
            if g > 0:
                table.append((int(m.group(1)), g))
    ratios = [x for x, n, mph in zip(det["Ratio"], det["Neutral"], det["ShownMPH"])
              if x and n == 0.0 and mph and mph > 8]
    if ratios:
        hist = Counter(int(x // 10) * 10 for x in ratios)
        peaks = [k for k, c in hist.most_common(12)]
        r.line("most common rpm/mph (10 bins)", ", ".join(str(p) for p in sorted(peaks)))
    if table:
        r.line("table", ", ".join(f"{g}:{v:.0f}" for g, v in sorted(table)))
    gears = [e for e in events if e[1] == "gear"]
    r.line("gear changes logged", str(len(gears)))
    if len(gears) > 2:
        dwell = [b[0] - a[0] for a, b in zip(gears, gears[1:])]
        short = sum(1 for d in dwell if d < 0.5)
        r.line("changes held under 0.5 s", f"{short}")
        r.verdict(short < 0.1 * len(gears), f"{short} of {len(gears)} gear changes flapped back within 0.5 s")


def section_air(r, det, events):
    r.section("9. Jumps, wheelies, stoppies",
              "0.3 g free fall is a jump; 10 deg lift for 250 ms is a wheelie (every threshold a guess)")
    for kind in ("takeoff", "land", "stunt", "perf"):
        evs = [e for e in events if e[1] == kind]
        r.line(kind, str(len(evs)))
        for t, _, kv in evs[:8]:
            r.note(f"{t:9.3f} s  " + " ".join(f"{k}={v}" for k, v in kv.items()))
    if det and det.n:
        lift = vals(det["LiftDeg"])
        if lift:
            r.line("lift p1 / p99 / max", f"{pct(lift, 1):+.1f} / {pct(lift, 99):+.1f} / {max(lift):+.1f} deg")
            r.note("plain riding should keep p99 well under the 10 deg start")


def section_gps(r, events, det, diag):
    r.section("10. GPS", "a fix within a minute, 10 Hz updates, few losses mid-ride")
    fixes = [e for e in events if e[1] == "fix"]
    gained = [e for e in fixes if e[2].get("fix") == "1"]
    lost = [e for e in fixes if e[2].get("fix") == "0"]
    r.line("first fix at", fmt(gained[0][0] if gained else None, 1, " s"))
    r.line("fixes lost", str(len(lost)))
    if det and det.n:
        new = sum(1 for v in det["GpsNew"] if v == 1.0)
        ts = vals(det["Time"])
        if len(ts) > 1 and ts[-1] > ts[0]:
            r.line("new fixes per second", f"{new / (ts[-1] - ts[0]):.1f}")
    if diag and diag.n:
        sats = [v for v in vals(diag["Sats"]) if v > 0]
        hd = [v for v in vals(diag["HDOP"]) if v > 0]
        if sats:
            r.line("sats p10 / p50", f"{pct(sats, 10):.0f} / {pct(sats, 50):.0f}")
        if hd:
            r.line("HDOP p50 / p90", f"{pct(hd, 50):.1f} / {pct(hd, 90):.1f}")
        bad = vals(diag["GnssBadCsum"])
        sen = vals(diag["GnssSentences"])
        if bad and sen and sen[-1]:
            r.line("NMEA checksum failures", f"{bad[-1]:.0f} of {sen[-1]:.0f}")


def section_system(r, diag, events):
    r.section("11. The machine", "heap and stacks hold, render keeps its rate, the pod bus is clean")
    if not diag or not diag.n:
        return
    # The first row can come before keeper has measured anything: all 0.
    measured = [i for i, h in enumerate(diag["HeapFree"])
                if isinstance(h, float) and h > 0]
    hm = [diag["HeapMin"][i] for i in measured if isinstance(diag["HeapMin"][i], float)]
    if hm:
        r.line("internal heap low-water", f"{min(hm) / 1024:.1f} kB")
    sl = [(diag["StackLowFree"][i], diag["StackLowTask"][i]) for i in measured
          if isinstance(diag["StackLowFree"][i], float) and diag["StackLowTask"][i]]
    if sl:
        low, task = min(sl, key=lambda x: x[0])
        r.line("tightest stack", f"{task} {low:.0f} B free")
        r.verdict(low > 256, f"tightest stack has {low:.0f} B left")
    rh = vals(diag["RenderHz"])
    if rh:
        r.line("render rate p10 / p50", f"{pct(rh, 10):.0f} / {pct(rh, 50):.0f} Hz")
    for c in ("ImuTimeouts", "PodRecoveries", "PodStuck"):
        v = vals(diag[c])
        if v:
            r.line(c, f"{v[-1] - v[0]:.0f} this ride")
    ct = vals(diag["ChipTempC"])
    if ct:
        r.line("P4 die temperature max", f"{max(ct):.1f} C")
    health = [e for e in events if e[1] == "health"]
    r.line("device state changes", str(len(health)))
    for t, _, kv in health[:30]:
        r.note(f"{t:9.3f} s  {kv.get('dev'):<12} {kv.get('state'):<9} err={kv.get('err')} faults={kv.get('faults')}")


def section_commands(r, events):
    cmds = [e for e in events if e[1] == "cmd"]
    r.section("12. What the rider did", "--")
    r.line("commands", str(len(cmds)))
    for t, _, kv in cmds[:40]:
        r.note(f"{t:9.3f} s  " + " ".join(f"{k}={v}" for k, v in kv.items()))


def plots(outdir, tables):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("ridelog: --plots needs matplotlib", file=sys.stderr)
        return
    os.makedirs(outdir, exist_ok=True)
    det, tach = tables.get("det"), tables.get("tach")
    if det and det.n:
        fig, ax = plt.subplots(4, 1, figsize=(14, 12), sharex=True)
        t = det["Time"]
        for c in ("FrontMPH", "RearMPH", "GpsMPH", "FusedMPH", "ShownMPH"):
            ax[0].plot(t, det[c], label=c, lw=0.8)
        ax[0].legend(); ax[0].set_ylabel("mph")
        for c in ("Lean", "AccelLean", "Steer"):
            ax[1].plot(t, det[c], label=c, lw=0.8)
        ax[1].legend(); ax[1].set_ylabel("deg")
        ax[2].plot(t, det["RPM"], lw=0.8); ax[2].set_ylabel("rpm")
        ax[3].plot(t, det["Ratio"], ".", ms=1); ax[3].set_ylabel("rpm/mph"); ax[3].set_ylim(0, 600)
        fig.savefig(os.path.join(outdir, "det.png"), dpi=110)
    if tach and tach.n:
        fig, ax = plt.subplots(3, 1, figsize=(14, 9), sharex=True)
        t = tach["Time"]
        ax[0].plot(t, tach["RawRPM"], lw=0.6); ax[0].set_ylabel("raw rpm")
        ax[1].plot(t, tach["TachRingUs"], ".", ms=2, label="ring us")
        ax[1].plot(t, tach["TachRejected"], ".", ms=2, label="rejected")
        ax[1].legend()
        ax[2].plot(t, tach["LampLatMaxUs"], ".", ms=2, label="edge->lamp us")
        ax[2].plot(t, tach["WakeLatMaxUs"], ".", ms=2, label="edge->ctrl us")
        ax[2].axhline(LAMP_BUDGET_US, color="r"); ax[2].legend()
        fig.savefig(os.path.join(outdir, "tach.png"), dpi=110)
    print(f"plots in {outdir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("card")
    ap.add_argument("--session", type=int)
    ap.add_argument("--plots")
    a = ap.parse_args()

    session, files = find_session(a.card, a.session)
    tables = {}
    for s, path in files.items():
        try:
            tables[s] = Table(path)
        except (OSError, csv.Error) as e:
            print(f"ridelog: {path}: {e}", file=sys.stderr)
    evt = tables.get("evt")
    events = parse_events(evt) if evt and evt.has("Detail") else []
    info = header_info(evt) if evt else {}
    print(f"session {session:03d}   firmware {info.get('firmware', '--')}")
    if not any(s in tables for s in STREAMS):
        print("no detail files: CONFIG -> Log -> Detail log was off, or this is an older card")

    r = Report()
    diag = tables.get("diag")
    section_files(r, files, tables, diag)
    section_power(r, info, events, diag)
    section_tach(r, info, tables.get("tach"), tables.get("det"))
    section_lamps(r, tables.get("tach"), diag)
    section_wheels(r, info, tables.get("tach"), tables.get("det"))
    section_fusion(r, tables.get("det"), events)
    section_imu(r, tables.get("imu"), tables.get("det"), info)
    section_lean(r, tables.get("det"), events)
    section_gear(r, tables.get("det"), events, info)
    section_air(r, tables.get("det"), events)
    section_gps(r, events, tables.get("det"), diag)
    section_system(r, diag, events)
    section_commands(r, events)

    print()
    print("=" * 78)
    bad = [v for v in r.verdicts if v[0] != "OK  "]
    print(f"{len(r.verdicts)} checks, {len(bad)} to look at")
    for tag, text in bad:
        print(f"  [{tag}] {text}")
    if a.plots:
        plots(a.plots, tables)


if __name__ == "__main__":
    main()
