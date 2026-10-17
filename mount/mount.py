"""Bare-PCB carrier + handlebar clamp for the dash board.

Frame: PCB-top view, X right, Y up (gerber coordinates, mm).
Z = 0 is the PCB's rider-side face; +Z points away from the rider.
All dimensions are mm. Run:  python mount.py   -> mount/out/*.stl, *.step
"""
import os
import cadquery as cq

# ---- board (from the gerbers) ----------------------------------------
BX0, BX1, BY0, BY1 = 7.62, 127.0, 12.7, 81.28
HOLES = [(10.922, 16.002), (123.698, 16.002), (123.698, 77.978), (10.922, 77.978)]
PCB_T = 1.6
CX, CY = (BX0 + BX1) / 2, (BY0 + BY1) / 2

# ---- tunables --------------------------------------------------------
BAR_D = 28.6          # MEASURE: handlebar diameter at the clamp point
BAR_CLEAR = 0.2       # printed-bore allowance on the diameter
SPACER = 15.0         # PCB top face to frame; must clear the tallest part
FRAME_T = 4.0
BAR_W = 7.0           # frame perimeter width
RIB_W = 12.0          # centre bar width
SPINE = 44.0          # length of the bar between the two Y vertices
BOSS_OD = 6.4         # post diameter; seat for the pad cuts into it
PILOT = 1.7           # M2 self-tapping pilot (PETG)
PAD_T = 1.5           # soft pad thickness per side, compressed (O-ring or foam weatherstrip)
PAD_OD = 6.0          # pad outer diameter; seat on the post end is cut to this
SEAT_D = 0.5          # depth of the pad seat in the post end
CLAMP_W = 44.0        # clamp length along the bar
WALL = 7.0            # barrel wall around the bar
SADDLE_H = 2.0        # gap between the centre bar's top and the barrel's underside before the saddle
LUG_R, LUG_H = 6.5, 7.0   # round bolt lugs
GUSSET_H = 11.0       # arm rib height at the vertex
FLARE = 3.5           # post-to-arm flare
CLAMP_BOLT_X = 15.0   # bolt x offset from centre
M5_CLEAR, M5_NUT_AF, M5_NUT_H = 5.5, 8.4, 4.2
SPLIT_GAP = 1.0       # base and cap never touch the bar's mid-plane, so tightening grips

z_top_pcb = PCB_T
z_f0 = z_top_pcb + SPACER
z_f1 = z_f0 + FRAME_T

def _arm(p, q, w0, w1):
    """Tapered bar from p (width w0) to q (width w1) in the frame plane, round-ended."""
    import math
    (x0, y0), (x1, y1) = p, q
    L = math.hypot(x1 - x0, y1 - y0)
    ux, uy = (x1 - x0) / L, (y1 - y0) / L
    nx, ny = -uy, ux
    quad = [(x0 + nx * w0 / 2, y0 + ny * w0 / 2), (x1 + nx * w1 / 2, y1 + ny * w1 / 2),
            (x1 - nx * w1 / 2, y1 - ny * w1 / 2), (x0 - nx * w0 / 2, y0 - ny * w0 / 2)]
    body = cq.Workplane("XY").workplane(offset=z_f0).polyline(quad).close().extrude(FRAME_T)
    for (x, y), w in (((x0, y0), w0), ((x1, y1), w1)):
        body = body.union(cq.Workplane("XY").workplane(offset=z_f0).center(x, y)
                          .circle(w / 2).extrude(FRAME_T))
    return body

def _gusset(p, q, height, thick=3.0):
    """Triangular rib standing on the arm: tallest at p, zero at q. Stiffens the arm without a box."""
    import math
    (x0, y0), (x1, y1) = p, q
    L = math.hypot(x1 - x0, y1 - y0)
    plane = cq.Plane(origin=(x0, y0, z_f1), xDir=((x1 - x0) / L, (y1 - y0) / L, 0),
                     normal=((y1 - y0) / L, -(x1 - x0) / L, 0))
    return (cq.Workplane(plane).polyline([(0, 0), (L, 0), (0, height)]).close()
            .extrude(thick / 2, both=True))

def frame():
    """>--< : a Y at each end of a centre bar. Each Y forks to the two holes on its side."""
    (_, yb), (_, _), (_, yt) = HOLES[0], HOLES[1], HOLES[3]
    vl, vr = (CX - SPINE / 2, CY), (CX + SPINE / 2, CY)
    f = _arm(vl, vr, RIB_W, RIB_W)
    for v, hx in ((vl, HOLES[0][0]), (vr, HOLES[1][0])):
        for hy in (yb, yt):
            f = f.union(_arm(v, (hx, hy), RIB_W * 0.8, BAR_W))
            f = f.union(_gusset(v, (hx, hy), GUSSET_H))
    for x, y in HOLES:
        z_end = z_top_pcb + PAD_T                      # post stops short of the board: only the pad touches it
        boss = (cq.Workplane("XY").workplane(offset=z_end).center(x, y)
                .circle(BOSS_OD / 2).extrude(z_f0 - z_end + FRAME_T / 2))
        # flare the post into the arm so it does not snap at the joint
        flare = cq.Workplane("XY").add(cq.Solid.makeCone(
            BOSS_OD / 2, BOSS_OD / 2 + FLARE, FLARE * 1.8,
            pnt=cq.Vector(x, y, z_f0 - FLARE * 1.8), dir=cq.Vector(0, 0, 1)))
        f = f.union(boss).union(flare)
        f = f.cut(cq.Workplane("XY").workplane(offset=z_end).center(x, y)
                  .circle(PILOT / 2).extrude(9))
        f = f.cut(cq.Workplane("XY").workplane(offset=z_end).center(x, y)   # seat that locates the pad
                  .circle(PAD_OD / 2).extrude(SEAT_D))
    return f

def clamp():
    """Round barrel around the bar, four round bolt lugs, and a tapered saddle down to the centre bar."""
    r = (BAR_D + BAR_CLEAR) / 2
    zc = z_f1 + SADDLE_H + r + WALL          # bar centreline height
    ro = r + WALL
    x0 = CX - CLAMP_W / 2
    def yz(off=0):          # sketch plane across the bar, extruded along X
        return cq.Workplane("YZ").workplane(offset=x0 + off)
    barrel = yz().center(CY, zc).circle(ro).extrude(CLAMP_W)
    saddle = (yz().polyline([(CY - RIB_W / 2, z_f0), (CY + RIB_W / 2, z_f0),
                             (CY + ro * 0.62, zc - ro * 0.78), (CY - ro * 0.62, zc - ro * 0.78)])
              .close().extrude(CLAMP_W))
    bolts = [(CX + sx * CLAMP_BOLT_X, CY + sy * (r + LUG_R * 0.6)) for sx in (-1, 1) for sy in (-1, 1)]
    lugs = None
    for x, y in bolts:
        lug = cq.Workplane("XY").workplane(offset=zc - LUG_H).center(x, y).circle(LUG_R).extrude(2 * LUG_H)
        lugs = lug if lugs is None else lugs.union(lug)
    body = barrel.union(lugs)
    body = body.edges("|X").edges(cq.selectors.BoxSelector((x0 - 1, -999, -999), (x0 + CLAMP_W + 1, 999, 999))).fillet(0.01) if False else body
    bore = (cq.Workplane("YZ").workplane(offset=x0 - 1).center(CY, zc).circle(r).extrude(CLAMP_W + 2))
    base_env = cq.Workplane("XY").workplane(offset=z_f0).center(CX, CY).rect(300, 300).extrude(zc - SPLIT_GAP / 2 - z_f0)
    cap_env = cq.Workplane("XY").workplane(offset=zc + SPLIT_GAP / 2).center(CX, CY).rect(300, 300).extrude(200)
    base = body.union(saddle).intersect(base_env).cut(bore)
    cap = body.intersect(cap_env).cut(bore)
    for x, y in bolts:
        thru = cq.Workplane("XY").workplane(offset=z_f0).center(x, y).circle(M5_CLEAR / 2).extrude(zc + ro + 10)
        base, cap = base.cut(thru), cap.cut(thru)
        trap = (cq.Workplane("XY").workplane(offset=zc - LUG_H - 0.1).center(x, y)
                .polygon(6, M5_NUT_AF / 0.866).extrude(M5_NUT_H + 0.1))
        base = base.cut(trap)
    return base, cap, zc

def ghost_pcb():
    pcb = cq.Workplane("XY").center(CX, CY).rect(BX1 - BX0, BY1 - BY0).extrude(PCB_T)
    for x, y in HOLES:
        pcb = pcb.cut(cq.Workplane("XY").center(x, y).circle(1.5).extrude(PCB_T))
    return pcb

def build():
    fr = frame()
    base, cap, zc = clamp()
    carrier = fr.union(base)          # one print: frame + clamp base
    return carrier, cap, zc

if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "out")
    os.makedirs(out, exist_ok=True)
    carrier, cap, zc = build()
    for name, part in (("carrier", carrier), ("clamp_cap", cap), ("pcb_ghost", ghost_pcb())):
        cq.exporters.export(part, f"{out}/{name}.stl")
        cq.exporters.export(part, f"{out}/{name}.step")
    bb = carrier.val().BoundingBox()
    print(f"carrier bbox {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f}; bar centre z={zc:.1f}")
