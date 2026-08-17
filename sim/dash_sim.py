#!/usr/bin/env python3
"""Desktop front end for the dash simulator.

Draws the panel, the eight shift lamps and the five handlebar buttons, and
gives you sliders for the things a bike would otherwise have to be moving to
produce.

The interesting half is the other side of the pipe: build-sim/dash_sim runs
the firmware's own screens, widgets, lap timer, delta matcher, button
debounce and shift-lamp maths. This file only draws pixels it is given and
sends back what you click, so nothing you see here is a mock-up of the
firmware -- it is the firmware.

    cmake -S sim -B build-sim && cmake --build build-sim
    python3 sim/dash_sim.py
"""

import os
import subprocess
import sys
import tkinter as tk

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(os.path.dirname(HERE), "build-sim", "dash_sim")

# Taken from the first frame's PPM header rather than hardcoded. The panel
# is landscape now, and a front end that assumes otherwise silently crops
# the right-hand third of the dash.
SCALE = 1

# Green, amber, red across the strip -- the same order as the lamps on the
# handlebar, so the window and the bike agree at a glance.
LAMP_COLOURS = ["#40c878"] * 3 + ["#ffb020"] * 2 + ["#ff4038"] * 3
LAMP_OFF = "#161a22"

BUTTONS = [
    ("A", "mode"),
    ("B", "screen"),
    ("C", "up"),
    ("D", "down"),
    ("E", "select"),
]

SLIDERS = [
    # name,      label,        from,   to,    resolution, default
    ("speed",   "Speed  mph",     0,   90,    1,    0),
    ("rpm",     "RPM",            0, 11000,  50,    0),
    ("lean",    "Lean  deg  (+ corner)", -55, 55, 1,   0),
    ("pitch",   "Pitch  deg",   -30,   30,    1,    0),
    ("tps",     "Throttle  %",    0,  100,    1,    0),
    ("coolant", "Coolant  C",     0,  130,    1,   82),
    ("vbat",    "Battery  V",     0,   15,  0.1, 12.6),
    ("sats",    "Satellites",     0,   16,    1,   11),
]

TOGGLES = [
    ("auto",    "Ride the circuit", False),
    ("neutral", "Neutral",          False),
    ("fix",     "GPS fix",          True),
    ("ecu",     "ECU online",       False),
]

COMMANDS = [
    ("lapline",   "Set finish line here"),
    ("laparm",    "Arm"),
    ("lapclear",  "Clear laps"),
    ("tripreset", "Reset trip A"),
    # Drops the accelerometer to zero and then spikes it, so the firmware's
    # own jump detector decides there was a jump. Nothing tells the dash.
    ("jump",      "Jump"),
]


class Sim:
    """The simulator process, and the pipe to it."""

    def __init__(self, path):
        if not os.path.exists(path):
            sys.exit(f"{path} not found -- build it first:\n"
                     f"  cmake -S sim -B build-sim && cmake --build build-sim")
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, bufsize=0)

    def send(self, line):
        try:
            self.p.stdin.write((line + "\n").encode())
            self.p.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def _read_exactly(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.p.stdout.read(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return bytes(buf)

    def _read_line(self):
        buf = bytearray()
        while True:
            c = self.p.stdout.read(1)
            if not c:
                return None
            if c == b"\n":
                return buf.decode(errors="replace")
            buf += c

    def read_frame(self):
        """Returns (ppm_bytes, lamp_mask, flash, mode, screen) or None."""
        while True:
            line = self._read_line()
            if line is None:
                return None
            if line.startswith("FRAME "):
                break
        parts = line.split()
        lamps = int(parts[1])
        flash = parts[2] == "1"
        mode, screen = parts[3], parts[4]

        magic = self._read_line()          # P6
        dims = self._read_line()           # 320 480
        maxval = self._read_line()         # 255
        if magic is None or dims is None or maxval is None:
            return None
        w, h = (int(v) for v in dims.split())
        data = self._read_exactly(w * h * 3)
        if data is None:
            return None
        header = f"P6\n{w} {h}\n255\n".encode()
        return header + data, lamps, flash, mode, screen

    def close(self):
        self.send("quit")
        try:
            self.p.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.p.kill()


class App:
    def __init__(self, root, sim):
        self.sim = sim
        self.root = root
        self.blink = False
        self.size = (0, 0)
        root.title("dash — simulator")
        root.configure(bg="#0d0f14")

        left = tk.Frame(root, bg="#0d0f14")
        left.grid(row=0, column=0, padx=12, pady=12, sticky="n")

        # --- shift lamps, above the panel as they are on the bar ---------
        self.lamps = []
        strip = tk.Frame(left, bg="#0d0f14")
        strip.pack(pady=(0, 8))
        for i in range(8):
            lamp = tk.Canvas(strip, width=30, height=18, bg=LAMP_OFF,
                             highlightthickness=1,
                             highlightbackground="#2c3440")
            lamp.grid(row=0, column=i, padx=2)
            self.lamps.append(lamp)

        # --- the panel ---------------------------------------------------
        self.canvas = tk.Canvas(left, width=480 * SCALE, height=320 * SCALE,
                                bg="black", highlightthickness=1,
                                highlightbackground="#2c3440")
        self.canvas.pack()
        self.photo = None
        self.image_id = self.canvas.create_image(0, 0, anchor="nw")

        self.status = tk.Label(left, text="", fg="#7884" "96", bg="#0d0f14",
                               font=("TkFixedFont", 9))
        self.status.pack(pady=(6, 0))

        # --- buttons -----------------------------------------------------
        pad = tk.Frame(left, bg="#0d0f14")
        pad.pack(pady=10)
        for i, (key, what) in enumerate(BUTTONS):
            b = tk.Button(pad, text=f"{key}\n{what}", width=6, height=2,
                          bg="#161a22", fg="#e2e8f0",
                          activebackground="#38aaff",
                          highlightbackground="#2c3440")
            b.grid(row=0, column=i, padx=3)
            # Press and release are sent separately so the firmware's own
            # debounce and long-press timing decide what the press was.
            b.bind("<ButtonPress-1>", lambda e, k=key: self.sim.send(f"btn {k} down"))
            b.bind("<ButtonRelease-1>", lambda e, k=key: self.sim.send(f"btn {k} up"))
        tk.Label(pad, text="hold for a long press", fg="#788496",
                 bg="#0d0f14", font=("TkDefaultFont", 8)).grid(
                     row=1, column=0, columnspan=5, pady=(4, 0))

        # --- controls ----------------------------------------------------
        right = tk.Frame(root, bg="#0d0f14")
        right.grid(row=0, column=1, padx=12, pady=12, sticky="n")

        for name, label, lo, hi, res, default in SLIDERS:
            tk.Label(right, text=label, fg="#788496", bg="#0d0f14",
                     anchor="w").pack(fill="x")
            s = tk.Scale(right, from_=lo, to=hi, resolution=res,
                         orient="horizontal", length=260,
                         bg="#0d0f14", fg="#e2e8f0", troughcolor="#161a22",
                         highlightthickness=0,
                         command=lambda v, n=name: self.sim.send(f"set {n} {v}"))
            s.set(default)
            s.pack(fill="x", pady=(0, 4))

        for name, label, default in TOGGLES:
            var = tk.IntVar(value=1 if default else 0)
            cb = tk.Checkbutton(
                right, text=label, variable=var, fg="#e2e8f0", bg="#0d0f14",
                selectcolor="#161a22", activebackground="#0d0f14",
                activeforeground="#e2e8f0", anchor="w",
                command=lambda n=name, v=var: self.sim.send(f"set {n} {v.get()}"))
            cb.pack(fill="x")
            self.sim.send(f"set {name} {var.get()}")

        tk.Label(right, text="", bg="#0d0f14").pack()
        for cmd, label in COMMANDS:
            tk.Button(right, text=label, bg="#161a22", fg="#e2e8f0",
                      highlightbackground="#2c3440",
                      command=lambda c=cmd: self.sim.send(f"cmd {c}")).pack(
                          fill="x", pady=2)

        root.protocol("WM_DELETE_WINDOW", self.quit)
        self.pump()

    def pump(self):
        frame = self.sim.read_frame()
        if frame is None:
            self.status.config(text="simulator exited")
            return
        ppm, lamps, flash, mode, screen = frame

        self.photo = tk.PhotoImage(data=ppm)
        # Follow whatever the simulator actually sent, so a change of
        # orientation in the firmware needs no change here.
        w, h = self.photo.width(), self.photo.height()
        if (w, h) != self.size:
            self.size = (w, h)
            self.canvas.config(width=w * SCALE, height=h * SCALE)
        self.canvas.itemconfig(self.image_id, image=self.photo)

        # Over the limit, everything flashes -- the caller owns the blink
        # rate, which is why core/shift has no notion of time in it.
        self.blink = not self.blink
        for i, lamp in enumerate(self.lamps):
            if flash:
                on = self.blink
            else:
                on = bool(lamps & (1 << i))
            lamp.configure(bg=LAMP_COLOURS[i] if on else LAMP_OFF)

        self.status.config(text=f"{mode} / {screen}")
        self.root.after(1, self.pump)

    def quit(self):
        self.sim.close()
        self.root.destroy()


def main():
    sim = Sim(SIM)
    root = tk.Tk()
    App(root, sim)
    root.mainloop()


if __name__ == "__main__":
    main()
