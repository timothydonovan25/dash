"""Open, isolated mount for the dash board: the board floats on four silicone
damping balls in front of a rigid H-frame on the handlebar. No housing.

    frame  (rigid, one print with the clamp's lower half)  ── 4 damping balls ──  tray + PCB + panel
    clamp_cap  (the clamp's upper half; the mount's angle is set by turning it on the bar)

Three prints: tray, frame, clamp_cap. The frame prints front face down, the
clamp half growing up from it with its bore open at the top: no supports.

Why balls, not foam or hard posts (docs/imu-vibration.md §6): idle's 1st engine
order is ~28 Hz, so the board must sit at fn ~15-20 Hz. On ~100 g, four gimbal
damping balls land there and damp well. Open means nothing stops the board but
the balls: a ball that pops out is a board on its cables.

Frame: board frame, gerber X right / Y up, mm. Z = 0 is the PCB's rider face
(panel and shift LEDs, bottom layer); the rider is at -Z. Y max is the LED edge,
the top. Run:  python pod.py  -> mount/out/pod_*.stl, *.step
"""
import math
import os
import cadquery as cq

# ---- board (from the gerbers and the pick-and-place) -------------------------
BX0, BX1, BY0, BY1 = 7.62, 127.0, 12.7, 81.28
HOLES = [(10.922, 16.002), (123.698, 16.002), (123.698, 77.978), (10.922, 77.978)]  # 3.0 mm NPTH
# U8's body is ~1.2 mm from the bottom-right hole: no post there can keep a wall on that side
POSTS = [h for h in HOLES if h != (123.698, 16.002)]
PCB_T = 1.6
CX, CY = (BX0 + BX1) / 2, (BY0 + BY1) / 2
LEDS_X = (61.24, 114.7)        # LED7..LED0 centres, bottom layer, y = 79.12
LED_Y, LED_L, LED_H = 79.12, 3.2, 1.9

# Top-side parts the tray must clear: ref, x, y, rot (pick-and-place), body L x W from the
# footprint, h above the PCB's top face with any mating plug. Every h is an estimate: MEASURE.
# open=True: nothing above it at all: vertical connectors, whose wires leave straight up
# and then away, and the terminals' screws.
PARTS = [
    ("U11 terminal+plug", 31.75, 16.25, 0, 7.6, 9.0, 16.0, True),
    ("U8 terminal+plug", 117.51, 16.25, 0, 7.6, 9.0, 16.0, True),
    ("F1 fuse holder", 91.0, 17.5, 180, 16.0, 6.7, 10.0, False),
    ("C23 220uF", 78.27, 17.3, 270, 6.6, 6.6, 8.0, False),
    ("SW2 slide switch", 105.76, 16.5, 0, 8.7, 3.7, 7.5, False),
    ("U16 module+IPEX", 67.82, 63.63, 0, 27.0, 27.0, 6.0, False),
    ("C22 10uF", 84.46, 25.91, 270, 5.3, 5.3, 5.6, False),
    ("L1 5.6uH", 107.95, 37.85, 180, 8.5, 8.0, 5.4, False),
    ("U4 GPS", 36.64, 72.57, 0, 16.0, 16.0, 5.0, False),
    ("JP1 u.FL+plug", 44.6, 61.77, 180, 3.5, 3.5, 5.0, False),
    ("CN1 GH vertical+plug", 40.77, 16.13, 0, 7.0, 6.0, 7.0, True),
    ("CN2 GH vertical+plug", 55.12, 16.26, 0, 5.8, 6.0, 7.0, True),
    ("CN3 GH vertical+plug", 64.14, 16.26, 0, 5.8, 6.0, 7.0, True),
    ("U6 opto", 39.5, 35.43, 0, 10.3, 4.6, 4.0, False),
    ("U9 USB-C", 11.81, 68.33, 270, 9.0, 7.4, 3.5, False),
    ("FPC1 + tail", 122.52, 48.28, 270, 25.0, 5.5, 3.0, False),
    ("U5 TO-252", 121.83, 70.97, 135, 10.0, 6.2, 2.5, False),
    ("CARD1 microSD", 17.02, 46.61, 270, 14.0, 12.0, 2.0, False),
    ("D4 SMB", 92.96, 25.02, 0, 5.3, 3.6, 2.5, False),
    ("D8 SMB", 49.02, 16.64, 90, 5.3, 3.6, 2.5, False),
    ("D9 SMB", 69.98, 16.64, 90, 5.3, 3.6, 2.5, False),
]
KEEP = 1.0           # air the tray keeps around every part, each way
WIRE_WINDOW = (27.0, 105.0, 21.5)  # x0, x1, y1: the bottom edge is open between the cross bars: wires
# from U11 and CN1-CN3 leave freely and F1 can be pulled; the screwed PCB closes the U
BALL_HEAD_D, BALL_HEAD_L = 10.0, 5.0   # MEASURE: ball head below the tray's plate, toward the board

# ---- panel: MEASURE it; the board has no outline for it ------------------------
PANEL_W, PANEL_H, PANEL_T = 85.0, 55.5, 3.0   # HT035SHV007NS module + tape; landscape
PANEL_CX, PANEL_CY = 81.0, 48.28              # tail centred on FPC1 (y), x a guess

# ---- tunables ---------------------------------------------------------------
SPACER = 15.0        # PCB top face to tray: must clear the tallest part incl. terminal plugs
TRAY_T = 4.0
TRAY_W = 7.0         # tray ring width, over the board's edge
PILOT_M25 = 2.2      # M2.5 self-tapping pilot (PETG), screws in from the rider side
DAMP = [(22.0, 30.0), (22.0, 64.0), (112.0, 30.0), (112.0, 64.0)]  # clear of parts > 6 mm
# Balls bought 2026-11-07: FOYTOKI "heavy duty" 10-pack. The listing's only size is
# 9 x 5 x 5 mm, which is not a gimbal ball: measure one before printing.
DAMP_HOLE = 3.25     # the hole the balls snap into; 6.5 printed far too loose (2026-11-07)
DAMP_GAP = 10.0      # MEASURE: plate face to plate face across a seated ball
DAMP_PLATE = 2.0     # plate thickness at the ball hole (the ball's neck length)
DAMP_CB = 12.0       # counterbore that thins a plate down to DAMP_PLATE
FRAME_T = 6.0        # H-frame thickness; ball holes are thinned to DAMP_PLATE
FRAME_W = 16.0       # H-frame leg width
# C6 antenna: RP-SMA female bulkhead (IPEX pigtail) through a flange on a riser above the
# board's top edge, axis +Y, so the thumb antenna stands up clear of the bar. Rigid side:
# the coax's slack loop crosses the isolation, like every other cable.
ANT_X = CX           # over U16's IPEX, between the clamp's lugs (x CX +-8.5)
ANT_Y1 = BY1 + 6.0   # flange top
ANT_T = 3.0          # MEASURE: flange thickness the bulkhead's thread takes with nut + washer
ANT_HOLE = 6.6       # 1/4"-36 thread
ANT_W = 16.0         # flange width, x
ANT_DEPTH = 18.0     # flange depth, z from the frame's front face
ANT_Z = FRAME_T + 6.0   # hole axis behind the frame's front face: the body clears the riser
ANT_BODY_D, ANT_BODY_L = 10.0, 12.0   # MEASURE: bulkhead body and crimp below the flange
# Sunshade: part of the tray, so it floats with the screen. A visor off the board's top
# edge, tilted up so the top rows stay visible from above, and two 45-degree cheeks that
# brace it. The tray prints back face down: the visor is a wall growing up, the cheeks'
# lower edges rise at 45 degrees, no supports. Tilt the dash on the bar to look at it square.
HOOD_L = 30.0        # visor length from the board's face toward the rider
HOOD_TILT = 20.0     # degrees up from the screen's normal
HOOD_T = 2.0         # wall thickness
HOOD_CHEEK = 16.0    # cheek reach down the sides: stops above FPC1's fold round the right edge (y < 61)
TIE = (32.5, 28.5, 4.5, 2.5)   # zip-tie slot x, y, w, h: ties U11/CN1-CN3's wires to the frame's bottom edge

BAR_D = 28.6         # MEASURE: handlebar diameter at the clamp (7/8" bar = 22.2)
BAR_RUBBER = 1.0     # inner-tube strip in the bore, compressed, per side: grip + no slip
CLAMP_W = 44.0
CLAMP_WALL = 6.0
CLAMP_BOLT_X = 15.0
LUG_R, LUG_H = 6.5, 7.0
SPLIT_GAP = 1.0
M5_CLEAR, M5_NUT_AF, M5_NUT_H = 5.5, 8.4, 4.2
BASE_W, BASE_D, BASE_T = 64.0, 44.0, 6.0   # pad under the clamp, spreading it over the frame
BASE_MIN = 8.0                       # base under the barrel

# ---- bolt bracket: mounts on the handlebar holders' bolts instead of the bar (mount-specs.txt) ----
# Replaces clamp_cap only; tray and frame are unchanged. Bike axes: forward (to the front wheel), up.
BOLT_SPAN = 98.4     # 3 7/8": the two holder bolts, across the bike; the panel's centre sits midway
BOLT_RAKE = 26.1     # bolt axes from vertical, heads leaning toward the rider
SCREEN_FWD, SCREEN_UP = 25.4, 50.8   # panel face centre from the midpoint of the bolt-head tops
SCREEN_TILT = 20.0   # the screen's normal, degrees above horizontal
HEAD_H = 8.0         # MEASURE: 12 mm hex (M8) flange head height; the bolts need PLATE_T more length
M8_CLEAR = 8.6
HEAD_CLEAR = 24.0    # socket room around each head, kept clear up the bolt axis
PLATE_T = 6.0        # plate trapped under the heads
BOSS_R = 12.0        # plate reach behind each bolt, toward the rider
HOLDER_FWD = 15.0    # MEASURE: holder cap's reach ahead of its bolt; nothing goes below its top there
BLOCK_T = 10.0       # the cap's stand-in on the frame's clamp half; the M5 heads seat on it
ARM_X = (69.0, 103.0)   # clear of CN2/CN3's wire column (x < 67.1) and the frame's right leg (x > 104)
NECK_R = 12.0        # fillet radius where the neck leaves the crossbar
CHAMFER = 10.0       # 45-degree cut on the arm's outside corner at the M5 face; stays clear of the M5 heads
ARM_Y = BY0 - 8.0   # arm's top below the board: the 2 mm float and 6 mm air

# ---- derived heights --------------------------------------------------------
Z_PANEL = -PANEL_T
Z_TRAY0 = PCB_T + SPACER
Z_TRAY1 = Z_TRAY0 + TRAY_T
Z_BACK_IN = Z_TRAY1 + DAMP_GAP       # ball plates: tray's back face .. frame's front face
Z_BACK_OUT = Z_BACK_IN + FRAME_T


def _box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def _cyl(x, y, z0, z1, d):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(d / 2).extrude(z1 - z0)


def _part(x, y, rot, L, W, z0, z1):
    return (cq.Workplane("XY").box(L, W, z1 - z0, centered=(True, True, False))
            .rotate((0, 0, 0), (0, 0, 1), rot).translate((x, y, z0)))


def chips(grow=0.0):
    """The parts as boxes; grown by `grow` and, if open, to the sky: the tray's keep-outs."""
    out = None
    for _, x, y, rot, L, W, h, sky in PARTS:
        top = 99 if (sky and grow) else PCB_T + h + grow
        p = _part(x, y, rot, L + 2 * grow, W + 2 * grow, PCB_T, top)
        out = p if out is None else out.union(p)
    return out


def ball_heads():
    return [_cyl(x, y, Z_TRAY1 - DAMP_PLATE - BALL_HEAD_L, Z_TRAY1 - DAMP_PLATE, BALL_HEAD_D) for x, y in DAMP]


def _ycyl(x, z, y0, y1, d):
    return cq.Workplane("XY").add(cq.Solid.makeCylinder(d / 2, y1 - y0, cq.Vector(x, y0, z), cq.Vector(0, 1, 0)))


def ant_body():
    """The bulkhead below the flange, toward the board: a keep-out, not a print."""
    y = ANT_Y1 - ANT_T
    return _ycyl(ANT_X, Z_BACK_IN + ANT_Z, y - ANT_BODY_L, y, ANT_BODY_D)


def ant_mount():
    """Riser off the cross bar, a flange standing back from it, two side webs. Prints with no supports."""
    zi, za = Z_BACK_IN, Z_BACK_IN + ANT_Z
    y0 = CY + BASE_D / 2 - 6          # riser starts inside the cross bar
    m = _box(ANT_X - ANT_W / 2, ANT_X + ANT_W / 2, y0, ANT_Y1, zi, zi + FRAME_T)
    m = m.union(_box(ANT_X - ANT_W / 2, ANT_X + ANT_W / 2, ANT_Y1 - ANT_T, ANT_Y1, zi, zi + ANT_DEPTH))
    for sx in (-1, 1):                # webs outside the bulkhead's hex
        x = ANT_X + sx * (ANT_W / 2 - 1.5)
        web = (cq.Workplane("YZ").workplane(offset=x - 1.5)
               .polyline([(ANT_Y1 - 12, zi + FRAME_T), (ANT_Y1, zi + FRAME_T), (ANT_Y1, zi + ANT_DEPTH)])
               .close().extrude(3.0))
        m = m.union(web)
    return m.cut(_ycyl(ANT_X, za, ANT_Y1 - ANT_T - 1, ANT_Y1 + 1, ANT_HOLE)).cut(ant_body())


def hood():
    """Visor from the tray's back face to HOOD_L in front of the board, and the two cheeks."""
    import math
    t = math.radians(HOOD_TILT)
    tip_y, tip_z = BY1 + HOOD_L * math.sin(t), -HOOD_L * math.cos(t)
    x0, x1 = BX0 - HOOD_T, BX1 + HOOD_T
    v = _box(x0, x1, BY1, BY1 + HOOD_T, 0, Z_TRAY1)          # beside the board, joined to the tray
    v = v.union(cq.Workplane("YZ").workplane(offset=x0)        # tilted visor in front of it
                .polyline([(BY1, 0), (BY1 + HOOD_T, 0), (tip_y + HOOD_T, tip_z), (tip_y, tip_z)])
                .close().extrude(x1 - x0))
    c = HOOD_CHEEK
    for xa in (x0, BX1):
        v = v.union(cq.Workplane("YZ").workplane(offset=xa)
                    .polyline([(BY1 + 0.3, 0), (BY1 + 0.3 + c * math.tan(t), -c), (BY1 + 0.3 - c, -c)]).close()
                    .extrude(HOOD_T))
    return v


def tray():
    """Rigid ring the PCB screws to, with two cross bars carrying the ball plates."""
    ring = (_box(BX0, BX1, BY0, BY1, Z_TRAY0, Z_TRAY1)
            .cut(_box(BX0 + TRAY_W, BX1 - TRAY_W, BY0 + TRAY_W, BY1 - TRAY_W, Z_TRAY0 - 1, Z_TRAY1 + 1)))
    for x in sorted({d[0] for d in DAMP}):
        ring = ring.union(_box(x - 7, x + 7, BY0, BY1, Z_TRAY0, Z_TRAY1))
    for x, y in DAMP:
        ring = ring.union(_cyl(x, y, Z_TRAY0, Z_TRAY1, DAMP_CB + 5))
    for x, y in POSTS:
        post = _cyl(x, y, PCB_T, Z_TRAY0 + 0.5, 6.4)
        flare = cq.Workplane("XY").add(cq.Solid.makeCone(
            3.2, 6.5, 6, pnt=cq.Vector(x, y, Z_TRAY0 - 6), dir=cq.Vector(0, 0, 1)))
        ring = ring.union(post).union(flare).cut(_cyl(x, y, PCB_T, PCB_T + 10, PILOT_M25))
    for x, y in DAMP:   # ball hole; counterbore from the board side leaves a DAMP_PLATE web at the back
        ring = (ring.cut(_cyl(x, y, Z_TRAY0 - 1, Z_TRAY1 + 1, DAMP_HOLE))
                .cut(_cyl(x, y, Z_TRAY0 - 1, Z_TRAY1 - DAMP_PLATE, DAMP_CB)))
    ring = ring.intersect(_box(BX0, BX1, BY0, BY1, 0, 99))   # flares stay inside the board outline
    ring = ring.cut(_box(WIRE_WINDOW[0], WIRE_WINDOW[1], BY0 - 1, WIRE_WINDOW[2], 0, 99))
    ring = ring.union(hood())
    return ring.cut(chips(KEEP))   # posts next to a part become D-shaped; check() guards their walls


def frame():
    """H: a leg under each pair of balls, a cross bar under the clamp plate."""
    xs = sorted({d[0] for d in DAMP})
    ys = [d[1] for d in DAMP]
    f = _box(xs[0], xs[1], CY - BASE_D / 2, CY + BASE_D / 2, Z_BACK_IN, Z_BACK_OUT)
    for x in xs:
        f = f.union(_box(x - FRAME_W / 2, x + FRAME_W / 2, min(ys), max(ys), Z_BACK_IN, Z_BACK_OUT))
    for x, y in DAMP:
        f = f.union(_cyl(x, y, Z_BACK_IN, Z_BACK_OUT, FRAME_W))
    f = f.edges("|Z").fillet(3)
    f = f.union(ant_mount())
    tx, ty, tw, th = TIE
    f = f.cut(_box(tx - tw / 2, tx + tw / 2, ty - th / 2, ty + th / 2, Z_BACK_IN - 1, Z_BACK_OUT + 1))
    for x, y in DAMP:   # ball holes; counterbore from the back leaves a DAMP_PLATE web
        f = (f.cut(_cyl(x, y, Z_BACK_IN - 1, Z_BACK_OUT + 1, DAMP_HOLE))
             .cut(_cyl(x, y, Z_BACK_IN + DAMP_PLATE, Z_BACK_OUT + 1, DAMP_CB)))
    base, _, _ = clamp()
    return f.union(base)


def _clamp_dims():
    """Bore radius, barrel radius, bore axis height and bolt positions of the clamp, in clamp() coordinates."""
    r = (BAR_D + 2 * BAR_RUBBER + 0.4) / 2
    ro = r + CLAMP_WALL
    by = r + LUG_R * 0.6
    return r, ro, BASE_MIN + ro, by, [(sx * CLAMP_BOLT_X, sy * by) for sx in (-1, 1) for sy in (-1, 1)]


def clamp():
    """Built on the frame's back (bar along X, base face z=0, +z away from the pod), then placed."""
    r, ro, zc, by, bolts = _clamp_dims()
    barrel = (cq.Workplane("YZ").workplane(offset=-CLAMP_W / 2).center(0, zc).circle(ro).extrude(CLAMP_W))
    for x, y in bolts:
        barrel = barrel.union(_cyl(x, y, zc - LUG_H, zc + LUG_H, 2 * LUG_R))
    plate = (cq.Workplane("XY").rect(BASE_W, BASE_D).extrude(BASE_T)
             .edges("|Z").fillet(6))
    # the plate narrows to the barrel: a tapered web, not a block
    web = (cq.Workplane("YZ").workplane(offset=-CLAMP_W / 2)
           .polyline([(-BASE_D / 2, 0), (BASE_D / 2, 0), (by + LUG_R, zc), (-by - LUG_R, zc)]).close()
           .extrude(CLAMP_W))
    ped = plate.union(web)
    base = barrel.union(ped).intersect(_box(-99, 99, -99, 99, -99, zc - SPLIT_GAP / 2))
    cap = barrel.intersect(_box(-99, 99, -99, 99, zc + SPLIT_GAP / 2, 99))
    bore = cq.Workplane("YZ").workplane(offset=-CLAMP_W).center(0, zc).circle(r).extrude(2 * CLAMP_W)
    base, cap = base.cut(bore), cap.cut(bore)
    for x, y in bolts:
        thru = _cyl(x, y, -50, 99, M5_CLEAR)
        base, cap = base.cut(thru), cap.cut(thru)
        zn = zc - LUG_H - 6     # side-entry nut slot; the nut pulls up, so keep meat above it
        base = base.cut(_box(x - M5_NUT_AF / 2 - 0.1, x + M5_NUT_AF / 2 + 0.1,
                             min(y, y * 9), max(y, y * 9), zn, zn + M5_NUT_H + 0.3))
    place = lambda p: p.translate((CX, CY, Z_BACK_OUT))
    bolt_len = 2 * LUG_H + 6 + 2   # cap top to the nut's far face, plus 2 mm
    return place(base), place(cap), bolt_len


def _bike():
    """The bolt bracket's geometry in board (Y, Z): bolt-head reference, bolt axis a (toward the head),
    and p, along the holder's top toward the front wheel. Board +Y and +Z as bike (forward, up)."""
    t, k = math.radians(SCREEN_TILT), math.radians(BOLT_RAKE)
    ey, ez = (math.sin(t), math.cos(t)), (math.cos(t), -math.sin(t))
    board = lambda f, u: (f * ey[0] + u * ey[1], f * ez[0] + u * ez[1])
    d = board(-SCREEN_FWD, -SCREEN_UP)
    ref = (PANEL_CY + d[0], Z_PANEL + d[1])
    a = board(-math.sin(k), math.cos(k))
    return ref, a, (-a[1], a[0])


def bolt_bracket():
    """Replaces clamp_cap: sits on the frame's clamp half (same four M5s and nuts), fills its half-bore,
    and carries an arm under the board to a plate trapped under the two handlebar-holder bolts."""
    r, _, zc, by, bolts = _clamp_dims()
    ref, a, p = _bike()
    at = lambda q, s, v: (q[0] + s * v[0], q[1] + s * v[1])
    t0 = at(ref, -HEAD_H, a)              # plate top, under the heads
    b0 = at(t0, -PLATE_T, a)              # plate bottom, on the holders
    b1 = at(b0, HOLDER_FWD, p)
    t1 = at(t0, (ARM_Y - t0[0]) / p[0], p)
    z_split = Z_BACK_OUT + zc - SPLIT_GAP / 2      # the frame's clamp half's top face
    z_top = z_split + BLOCK_T
    y_web = CY - by - LUG_R                        # the clamp half's widest point, at its top
    # 45-degree chamfer on the outside corner (ARM_Y, z_top): from c0 on the M5 face along (-1, -1) to the arm's back edge
    c0 = (ARM_Y + CHAMFER, z_top)
    dy, dz = b1[0] - ARM_Y, b1[1] - z_top
    u = CHAMFER / (dy - dz)                        # c0 + s(-1,-1) = (ARM_Y, z_top) + u(dy, dz)
    c1 = (ARM_Y + u * dy, z_top + u * dz)
    arm = (cq.Workplane("YZ").workplane(offset=ARM_X[0]).polyline([
        t0, t1, (ARM_Y, Z_TRAY1 + 6),   # then straight to the block: nothing fills the wedge under the board's edge
        (y_web - 1, z_split), (CY, z_split), (CY, z_top), c0, c1, b1, b0]).close()
        .extrude(ARM_X[1] - ARM_X[0]))
    x0 = CX - CLAMP_W / 2
    block = _box(x0, ARM_X[1], y_web, CY + by + LUG_R, z_split, z_top)
    plug = (cq.Workplane("YZ").workplane(offset=x0).center(CY, Z_BACK_OUT + zc).circle(r - 0.3)
            .extrude(CLAMP_W).intersect(_box(x0, x0 + CLAMP_W, 0, 99, 0, z_split)))
    pl = cq.Plane(origin=(PANEL_CX, b0[0], b0[1]), xDir=(1, 0, 0), normal=(0, a[0], a[1]))  # local y = -p
    plate = (cq.Workplane(pl).center(0, (BOSS_R - HOLDER_FWD) / 2)
             .rect(BOLT_SPAN + 2 * BOSS_R, BOSS_R + HOLDER_FWD).extrude(PLATE_T))
    # concave fillets where the neck leaves the crossbar, in the plate's plane (the neck is plate-thick there)
    rf = min(NECK_R, (ARM_Y - t0[0]) / p[0] - HOLDER_FWD)
    xa, xb, e = ARM_X[0] - PANEL_CX, ARM_X[1] - PANEL_CX, -HOLDER_FWD
    flare = (cq.Workplane(pl).center((xa + xb) / 2, e - rf / 2 + 0.25).rect(xb - xa + 2 * rf, rf + 0.5)
             .extrude(PLATE_T)
             .cut(cq.Workplane(pl).pushPoints([(xa - rf, e - rf), (xb + rf, e - rf)]).circle(rf).extrude(PLATE_T)))
    b = block.union(plug).union(arm).union(plate).union(flare)
    for x, y in bolts:
        b = b.cut(_cyl(CX + x, CY + y, z_split - 20, z_top + 1, M5_CLEAR))
    holes = cq.Workplane(pl).pushPoints([(-BOLT_SPAN / 2, 0), (BOLT_SPAN / 2, 0)])
    b = b.cut(holes.circle(M8_CLEAR / 2).extrude(PLATE_T))
    b = b.cut(cq.Workplane(pl).workplane(offset=PLATE_T).pushPoints([(-BOLT_SPAN / 2, 0), (BOLT_SPAN / 2, 0)])
              .circle(HEAD_CLEAR / 2).extrude(80))     # socket room up each bolt's axis
    zn = zc - LUG_H - 6
    return b, z_top - Z_BACK_OUT - zn + 2


def holders():
    """Keep-out: the holder caps, below the plate from behind the bolts to HOLDER_FWD ahead of them."""
    ref, a, p = _bike()
    b0y, b0z = ref[0] - (HEAD_H + PLATE_T) * a[0], ref[1] - (HEAD_H + PLATE_T) * a[1]
    pl = cq.Plane(origin=(PANEL_CX, b0y, b0z), xDir=(1, 0, 0), normal=(0, -a[0], -a[1]))  # local y = +p
    return cq.Workplane(pl).center(0, (HOLDER_FWD - 40) / 2).rect(200, HOLDER_FWD + 40).extrude(60)


def ghost():
    pcb = _box(BX0, BX1, BY0, BY1, 0, PCB_T)
    for x, y in HOLES:
        pcb = pcb.cut(_cyl(x, y, -1, 3, 3.0))
    panel = _box(PANEL_CX - PANEL_W / 2, PANEL_CX + PANEL_W / 2,
                 PANEL_CY - PANEL_H / 2, PANEL_CY + PANEL_H / 2, Z_PANEL, 0)
    leds = None
    for i in range(8):
        x = LEDS_X[0] + i * (LEDS_X[1] - LEDS_X[0]) / 7
        led = _box(x - LED_L / 2, x + LED_L / 2, LED_Y - 1.4, LED_Y + 1.4, -LED_H, 0)
        leds = led if leds is None else leds.union(led)
    return pcb, panel, leds


def _vol(a, b):
    return a.intersect(b).val().Volume()


def check(parts):
    """Fail the build if anything shades the screen or the LEDs, or the float has no room."""
    tr, fr, cap = parts
    pcb, panel, leds = ghost()
    board = tr.union(pcb).union(panel).union(leds)
    rigid = fr.union(cap).union(ant_body())
    # 1. the view: extrude the panel and every LED toward the rider, through everything
    for name, g in (("panel", panel), ("leds", leds)):
        bb = g.val().BoundingBox()
        ray = _box(bb.xmin, bb.xmax, bb.ymin, bb.ymax, bb.zmin - 50, bb.zmin)
        v = _vol(ray, rigid.union(tr))
        assert v < 1e-3, f"{name} is obscured: {v:.1f} mm^3 in its line of sight"
    # 2. the chips: the tray's cuts keep KEEP by construction; the ball heads must too,
    #    and a post cut D-shaped must still hold its screw with >= 1 mm of wall
    keep = chips(KEEP)
    for i, h in enumerate(ball_heads()):
        assert _vol(h, keep) < 1e-3, f"ball head {DAMP[i]} is within {KEEP} mm of a part"
    for x, y in POSTS:
        ring = _cyl(x, y, PCB_T, Z_TRAY0, PILOT_M25 + 2).cut(_cyl(x, y, PCB_T - 1, Z_TRAY0 + 1, PILOT_M25))
        assert ring.val().Volume() - _vol(ring, tr) < 1e-3, f"post at {(x, y)} has under 1 mm wall"
    # 3. the wires: nothing rigid above an open part either, all the way up
    for ref, x, y, rot, L, W, h, sky in PARTS:
        if sky:
            v = _vol(_part(x, y, rot, L, W, PCB_T + h, 99), rigid)
            assert v < 1e-3, f"{ref}: frame or clamp above it, {v:.1f} mm^3"
    # 4. the float: the board must clear the rigid parts at rest and at the travel limit's start
    for dx, dy, dz in ((0, 0, 0), (2, 0, 0), (-2, 0, 0), (0, 2, 0), (0, -2, 0), (0, 0, -2), (0, 0, 2)):
        v = _vol(board.translate((dx, dy, dz)), rigid)
        assert v < 1e-3, f"board hits the frame moved by {(dx, dy, dz)}: {v:.1f} mm^3"
    # 5. the bracket: a separate print beside the frame, and out of the holder caps it sits on
    assert _vol(cap, fr) < 1e-3, f"bracket overlaps the frame: {_vol(cap, fr):.1f} mm^3"
    assert _vol(cap, holders()) < 1e-3, f"bracket is inside the holders: {_vol(cap, holders()):.1f} mm^3"


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    bracket, bolt_len = bolt_bracket()
    parts = (tray(), frame(), bracket)
    check(parts)
    pcb, panel, leds = ghost()
    parts_ghost = chips()
    names = ("tray", "frame", "bolt_bracket")
    for name, part in list(zip(names, parts)) + [("ghost", pcb.union(panel).union(leds).union(parts_ghost))]:
        cq.exporters.export(part, f"{out}/pod_{name}.stl")
        cq.exporters.export(part, f"{out}/pod_{name}.step")
    for name, part in zip(names, parts):
        bb = part.val().BoundingBox()
        print(f"{name:11s} {bb.xlen:6.1f} x {bb.ylen:6.1f} x {bb.zlen:5.1f} mm  {part.val().Volume() / 1000 * 1.27:5.1f} g solid")
    print(f"clamp bolts M5 x >= {bolt_len:.0f} mm; checks passed")
