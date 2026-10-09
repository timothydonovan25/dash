#!/usr/bin/env python3
"""Plot a `replay --out` file as an SVG, with nothing to install.

    python3 tools/replay/plot.py out.csv [out.svg]

Shows truth (synthetic rides only), what the dash shows today, the fused
speed with its 1-sigma band, the raw sources, and underneath which sources
the filter used and rejected. Open the SVG in a browser.
"""
import csv
import sys

W, H, PAD = 1200, 640, 50
TOP_H = 440          # speed panel height; the source strip goes below


def num(row, key):
    try:
        return float(row.get(key, ""))
    except ValueError:
        return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rows = list(csv.DictReader(open(sys.argv[1])))
    out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].rsplit(".", 1)[0] + ".svg"
    t = [num(r, "t") for r in rows]
    t0, t1 = min(t), max(t) or 1.0
    vals = [v for r in rows for k in ("truth", "raw", "fused", "front", "rear", "gps")
            for v in [num(r, k)] if v is not None and v >= 0]
    vmax = max(vals + [10.0]) * 1.08

    def x(s):
        return PAD + (s - t0) / (t1 - t0) * (W - 2 * PAD)

    def y(v):
        return PAD + TOP_H - v / vmax * TOP_H

    def line(key, color, width, dash="", opacity=1.0):
        pts = [(x(a), y(num(r, key))) for a, r in zip(t, rows)
               if num(r, key) is not None and num(r, key) >= 0]
        if not pts:
            return ""
        d = " ".join(f"{px:.1f},{py:.1f}" for px, py in pts)
        return (f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
                f'stroke-dasharray="{dash}" opacity="{opacity}"/>')

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'font-family="sans-serif" font-size="12"><rect width="100%" height="100%" fill="#fff"/>']
    # grid
    step = 10 if vmax > 30 else 5
    v = 0
    while v < vmax:
        svg.append(f'<line x1="{PAD}" x2="{W-PAD}" y1="{y(v):.1f}" y2="{y(v):.1f}" stroke="#eee"/>'
                   f'<text x="{PAD-6}" y="{y(v)+4:.1f}" text-anchor="end" fill="#666">{v}</text>')
        v += step
    s = int(t0)
    while s <= t1:
        svg.append(f'<text x="{x(s):.1f}" y="{H-8}" text-anchor="middle" fill="#666">{s}s</text>')
        s += 5
    # sigma band
    band = [(x(a), y(num(r, "fused") + num(r, "sigma"))) for a, r in zip(t, rows)] + \
           [(x(a), y(max(0.0, num(r, "fused") - num(r, "sigma")))) for a, r in reversed(list(zip(t, rows)))]
    svg.append('<polygon points="' + " ".join(f"{a:.1f},{b:.1f}" for a, b in band) +
               '" fill="#d62728" opacity="0.15"/>')
    svg += [line("truth", "#000", 4, opacity=0.25), line("front", "#1f77b4", 1, opacity=0.6),
            line("rear", "#ff7f0e", 1, opacity=0.6), line("raw", "#555", 1.2, "6,4"),
            line("fused", "#d62728", 1.8)]
    for a, r in zip(t, rows):
        g = num(r, "gps")
        if g is not None:
            svg.append(f'<circle cx="{x(a):.1f}" cy="{y(g):.1f}" r="1.6" fill="#2ca02c"/>')
    # legend
    for i, (name, color) in enumerate([("truth", "#bbb"), ("front wheel", "#1f77b4"),
                                       ("rear wheel", "#ff7f0e"), ("GPS Doppler", "#2ca02c"),
                                       ("dash today", "#555"), ("fused ±1σ", "#d62728")]):
        lx = W - PAD - 130
        ly = PAD + 10 + i * 16
        svg.append(f'<rect x="{lx}" y="{ly-8}" width="14" height="4" fill="{color}"/>'
                   f'<text x="{lx+20}" y="{ly-2}">{name}</text>')
    # source strip: used | , rejected x
    base = PAD + TOP_H + 30
    for i, (name, bit) in enumerate([("front", 2), ("rear", 4), ("GPS", 8)]):
        yy = base + i * 22
        svg.append(f'<text x="{PAD-6}" y="{yy+4}" text-anchor="end">{name}</text>')
        for a, r in zip(t, rows):
            if int(r["used"]) & bit:
                svg.append(f'<line x1="{x(a):.1f}" x2="{x(a):.1f}" y1="{yy-5}" y2="{yy+5}" stroke="#2ca02c" stroke-width="0.6"/>')
            elif int(r["rejected"]) & bit:
                svg.append(f'<text x="{x(a):.1f}" y="{yy+4}" fill="#d62728" text-anchor="middle" font-size="9">x</text>')
    svg.append(f'<text x="{PAD}" y="{base-14}" fill="#666">sources: green used, red x rejected</text></svg>')
    open(out, "w").write("\n".join(svg))
    print(out)


if __name__ == "__main__":
    main()
