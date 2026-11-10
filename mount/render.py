"""Render the pod on its bolt bracket in bike axes: x forward, y left, z up, origin = bolt-head tops' midpoint.
Run:  python render.py  -> out/pod_bracket_render.png. Needs pyvista."""
import math, os, sys
import numpy as np, pyvista as pv, cadquery as cq
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pod

ref, a, p = pod._bike()
t = math.radians(pod.SCREEN_TILT)
M = np.eye(4)   # board -> bike; board +X is the rider's left
M[:3, :3] = [[0, math.sin(t), math.cos(t)], [1, 0, 0], [0, math.cos(t), -math.sin(t)]]
M[:3, 3] = -M[:3, :3] @ np.array([pod.PANEL_CX, ref[0], ref[1]])

def mesh(w):
    v, f = w.val().tessellate(0.05, 0.2)
    pts = np.array([x.toTuple() for x in v])
    return pv.PolyData(pts, np.hstack([[3, *tri] for tri in f])).transform(M, inplace=False)

pcb, panel, leds = pod.ghost()
bracket, _ = pod.bolt_bracket()
# the bike's side: two holder bolts (head + shank) and the holder caps they clamp, as context
b0 = (ref[0] - (pod.HEAD_H + pod.PLATE_T) * a[0], ref[1] - (pod.HEAD_H + pod.PLATE_T) * a[1])
pl = cq.Plane(origin=(pod.PANEL_CX, ref[0], ref[1]), xDir=(1, 0, 0), normal=(0, -a[0], -a[1]))
xs = [(-pod.BOLT_SPAN / 2, 0), (pod.BOLT_SPAN / 2, 0)]
bolts = (cq.Workplane(pl).pushPoints(xs).polygon(6, 13.9).extrude(pod.HEAD_H)
         .union(cq.Workplane(pl).pushPoints(xs).circle(4).extrude(pod.HEAD_H + pod.PLATE_T + 35)))
plh = cq.Plane(origin=(pod.PANEL_CX, *b0), xDir=(1, 0, 0), normal=(0, -a[0], -a[1]))   # local y = +p
caps = cq.Workplane(plh).pushPoints([(x, (pod.HOLDER_FWD - 35) / 2) for x, _ in xs]).rect(34, 35 + pod.HOLDER_FWD).extrude(22)

parts = [(pod.tray(), "#c47a2c", 1), (pod.frame(), "#2a2a2a", 1), (bracket, "#3d7fc4", 1),
         (pcb, "#1d5a2c", 1), (panel, "#0a0a0a", 1), (leds, "#d02020", 1), (pod.chips(), "#9a9a9a", 1),
         (bolts, "#c8c8c8", 1), (caps, "#808080", 0.35)]
meshes = [(mesh(w), c, o) for w, c, o in parts]

pc = (M @ np.array([pod.PANEL_CX, pod.PANEL_CY, pod.Z_PANEL, 1]))[:3]
views = [("side, from the bike's right: front to the right (true angles)", "side"),
         ("rider's eye", "rider"),
         ("below, front three-quarter: the arm", "below")]
pl3 = pv.Plotter(off_screen=True, shape=(1, 3), window_size=(2400, 900))
for i, (title, v) in enumerate(views):
    pl3.subplot(0, i)
    pl3.set_background("white")
    for m, c, o in meshes:
        pl3.add_mesh(m, color=c, opacity=o, smooth_shading=False)
    pl3.add_text(title, font_size=12, color="black")
    if v == "side":
        n = np.array([-math.cos(t), 0, math.sin(t)])
        Y = -80   # in front of everything, toward the camera
        pl3.add_lines(np.array([[0, Y, 0], [pc[0], Y, 0], [pc[0], Y, 0], [pc[0], Y, pc[2]],
                                [*(pc + [0, Y, 0])], [*(pc + [0, Y, 0] + 45 * n)],
                                [-90, Y, 0], [150, Y, 0]]), color="red", width=3)
        pl3.add_point_labels([[-75, Y, 8], [pc[0] - 30, Y, pc[2] / 2 + 10], [*(pc + [0, Y, 0] + 52 * n)]],
                             [f"bolt heads; {pc[0]/25.4:.2f} in fwd", f"{pc[2]/25.4:.2f} in up", f"{pod.SCREEN_TILT:.0f} deg"],
                             font_size=14, text_color="red", shape=None, show_points=False, always_visible=True)
        pl3.camera_position = [(30, -600, 10), (30, 0, 10), (0, 0, 1)]
        pl3.enable_parallel_projection()
    elif v == "rider":
        pl3.camera_position = [(-450, 0, 330), (30, 0, 20), (0, 0, 1)]
    else:
        pl3.camera_position = [(380, -300, -260), (40, 0, 0), (0, 0, 1)]
    pl3.reset_camera_clipping_range()
png = os.path.join(HERE, "out", "pod_bracket_render.png")
pl3.screenshot(png)
print("wrote", png)
