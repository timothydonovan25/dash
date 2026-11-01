#!/usr/bin/env python3
"""Watch the board's attitude in 3D, live from its USB console.

The firmware prints `IMU lean pitch heading g_lon g_lat g_vert yaw temp timeouts`
at 20 Hz while a USB host is connected (keeper_service.cpp). This draws the
board turned by the firmware's own lean, pitch and heading, so a sign or axis
error shows as the drawing moving the wrong way.

Frame (core/attitude/attitude.hpp): x forward, y LEFT, z up. The board stands
upright facing the rider (icm42688p.cpp): forward is out of the component
side, up is the LED edge, left is the gerber +X edge. Lean + is right, pitch
+ is nose up, heading + is a right turn.

Run with ESP-IDF's Python (it has pyserial and Tk):
    ~/.espressif/python_env/idf6.0_py3.14_env/bin/python tools/imu_view.py [-p PORT]
Opening the port does not reset the board.
"""
import argparse
import glob
import math
import queue
import threading
import time
import tkinter as tk

import serial

# Board, mm, from the gerbers; centred, in board axes (gerber X, Y, and Z out of
# the component side). to_bike() stands it up the way icm42688p.cpp maps it.
BW, BH, BT = 119.4, 68.6, 1.6
LEDS_X = [61.24 + i * (114.7 - 61.24) / 7 - 67.31 for i in range(8)]
LED_Y = 79.12 - 46.99
MODULE = (67.82 - 67.31, 63.63 - 46.99, 27.0)   # U16 on the component side
SCALE = 3.0


def rot(lean, pitch, heading):
    """Body to world. Roll right = +x rotation, nose up = -y, right turn = -z."""
    f, t, p = (math.radians(a) for a in (lean, -pitch, -heading))
    cf, sf, ct, st, cp, sp = math.cos(f), math.sin(f), math.cos(t), math.sin(t), math.cos(p), math.sin(p)
    rx = ((1, 0, 0), (0, cf, -sf), (0, sf, cf))
    ry = ((ct, 0, st), (0, 1, 0), (-st, 0, ct))
    rz = ((cp, -sp, 0), (sp, cp, 0), (0, 0, 1))
    mul = lambda a, b: tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))
    return mul(rz, mul(ry, rx))


def apply(r, v):
    return tuple(sum(r[i][k] * v[k] for k in range(3)) for i in range(3))


def view(v):
    """World to screen: camera behind-left of the bike, above, looking forward-down."""
    x, y, z = v
    az, el = math.radians(-35), math.radians(25)
    # turn the world so the camera looks along +x' , then tilt down
    x1 = x * math.cos(az) - y * math.sin(az)
    y1 = x * math.sin(az) + y * math.cos(az)
    z1 = z
    depth = x1 * math.cos(el) - z1 * math.sin(el)     # along the view, forward and down
    up = x1 * math.sin(el) + z1 * math.cos(el)
    return (-y1, up, depth)        # screen right = world -y' (the bike's right)


def reader(port, q, stop):
    while not stop.is_set():
        p = port or next(iter(sorted(glob.glob("/dev/ttyACM*"))), None)
        if not p:
            q.put(("status", "waiting for /dev/ttyACM*")); time.sleep(0.5); continue
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = p, 115200, 0.2
            # Both high as the kernel opens it, then RTS down before DTR: DTR
            # low with RTS high is the USB-Serial-JTAG's reset (pod_keys.py).
            s.dtr = s.rts = True
            s.open()
            s.rts = False
            s.dtr = False
            q.put(("status", f"{p}: connected, waiting for IMU lines"))
            buf = b""
            while not stop.is_set():
                buf += s.read(4096)
                *lines, buf = buf.split(b"\n")
                for ln in lines:
                    parts = ln.decode("ascii", "replace").split()
                    if len(parts) == 10 and parts[0] == "IMU":
                        try:
                            q.put(("imu", [float(x) for x in parts[1:]]))
                        except ValueError:
                            pass
        except (serial.SerialException, OSError) as e:
            q.put(("status", f"{p}: {e}; retrying"))
            time.sleep(0.5)


class App:
    def __init__(self, root, port):
        self.root = root
        root.title("Dash IMU")
        self.c = tk.Canvas(root, width=760, height=520, bg="#15181c", highlightthickness=0)
        self.c.pack(fill="both", expand=True)
        bar = tk.Frame(root, bg="#15181c"); bar.pack(fill="x")
        tk.Button(bar, text="Zero heading", command=self.zero).pack(side="left", padx=6, pady=4)
        self.status = tk.Label(bar, text="", fg="#9aa0a6", bg="#15181c", anchor="w")
        self.status.pack(side="left", fill="x", expand=True)
        self.q, self.stop = queue.Queue(), threading.Event()
        threading.Thread(target=reader, args=(port, self.q, self.stop), daemon=True).start()
        self.data, self.h0, self.last = None, 0.0, 0.0
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.tick()

    def zero(self):
        if self.data: self.h0 = self.data[2]

    def close(self):
        self.stop.set(); self.root.destroy()

    def tick(self):
        while not self.q.empty():
            kind, v = self.q.get()
            if kind == "status": self.status.config(text=v)
            else: self.data, self.last = v, time.time()
        self.draw()
        self.root.after(30, self.tick)

    def draw(self):
        c = self.c; c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        cx, cy = w * 0.42, h * 0.55
        lean, pitch, head, glon, glat, gvert, yaw, temp, tmo = self.data or [0] * 9
        r = rot(lean, pitch, head - self.h0)
        to_bike = lambda v: (v[2], v[0], v[1])     # board (X, Y, Z) -> bike (fwd, left, up)
        P = lambda v: view(apply(r, to_bike(v)))
        Pb = lambda v: view(apply(r, v))
        S = lambda s: (cx + s[0] * SCALE, cy - s[1] * SCALE)

        # ground grid, fixed in the world
        for i in range(-4, 5):
            for a, b in (((i * 25, -100, -40), (i * 25, 100, -40)), ((-100, i * 25, -40), (100, i * 25, -40))):
                c.create_line(*S(view(a)), *S(view(b)), fill="#262b31")

        # the slab: top (component side, z up) green, bottom (screen side) dark
        hx, hy, hz = BW / 2, BH / 2, BT / 2 + 2
        corners = {(sx, sy, sz): (sx * hx, sy * hy, sz * hz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)}
        faces = [((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1), "#2e7d4f", "top"),
                 ((-1, -1, -1), (-1, 1, -1), (1, 1, -1), (1, -1, -1), "#0e1013", "bottom"),
                 ((1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1), "#c0392b", "front"),
                 ((-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1), "#1f5a39", "back"),
                 ((-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1), "#24704a", "left"),
                 ((-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1), "#24704a", "right")]
        polys = []   # a convex slab: farthest face first is enough
        for *ks, col, name in faces:
            pts = [P(corners[k]) for k in ks]
            polys.append((sum(p[2] for p in pts) / 4, pts, col, name))
        for _, pts, col, name in sorted(polys, key=lambda t: -t[0]):
            c.create_polygon(*[q for p in pts for q in S(p)], fill=col, outline="#0a0c0e")
            if name == "top":
                mx, my, ms = MODULE
                sq = [P((mx + dx * ms / 2, my + dy * ms / 2, hz + 0.1)) for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
                c.create_polygon(*[q for p in sq for q in S(p)], fill="#8a9097", outline="")
            if name == "bottom":
                pw, ph = 85 / 2, 55.5 / 2
                sq = [P((81 - 67.31 + dx * pw, 48.28 - 46.99 + dy * ph, -hz - 0.1)) for dx, dy in ((-1, -1), (-1, 1), (1, 1), (1, -1))]
                c.create_polygon(*[q for p in sq for q in S(p)], fill="#000", outline="#333")
                for x in LEDS_X:
                    px, py = S(P((x, LED_Y, -hz - 0.1)))
                    c.create_oval(px - 3, py - 3, px + 3, py + 3, fill="#ff3030", outline="")

        # body axes from the centre
        for v, col, lab in (((90, 0, 0), "#ff5050", "x fwd"), ((0, 60, 0), "#50d070", "y left"), ((0, 0, 45), "#5090ff", "z up")):
            a, b = S(Pb((0, 0, 0))), S(Pb(v))
            c.create_line(*a, *b, fill=col, width=3, arrow="last")
            c.create_text(b[0] + 6, b[1], text=lab, fill=col, anchor="w", font=("Sans", 10, "bold"))

        stale = self.data is None or time.time() - self.last > 1.0
        txt = ("no data" if self.data is None else
               f"lean   {lean:+7.1f}°  (+ right)\npitch  {pitch:+7.1f}°  (+ nose up)\n"
               f"head   {head - self.h0:+7.1f}°  (+ right turn)\nyaw    {yaw:+7.2f} °/s\n\n"
               f"g lon  {glon:+6.3f}\ng lat  {glat:+6.3f}\ng vert {gvert:+6.3f}\n"
               f"|g|    {math.sqrt(glon*glon + glat*glat + gvert*gvert):6.3f}\n\n"
               f"IMU    {temp:5.1f} °C\ntimeouts {int(tmo)}")
        c.create_text(w - 16, 16, text=txt, fill="#ff8080" if stale else "#e6e6e6", anchor="ne", font=("Monospace", 12))
        c.create_text(16, 16, anchor="nw", fill="#9aa0a6", font=("Sans", 10),
                      text="screen (black, LED dots) faces you\ngreen = component side, forward\nred edge = gerber +X, your left")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", help="serial port (default: first /dev/ttyACM*)")
    args = ap.parse_args()
    root = tk.Tk()
    App(root, args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
