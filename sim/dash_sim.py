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

    python3 sim/dash_sim.py --pod /dev/ttyACM0

--pod reads a real handlebar pod through an Arduino that prints a
"GPIOA=0x.." line whenever the MCP23017's port A changes (pressed = 1), so
the physical buttons drive the sim through the same debounce as a click.
"""

import argparse
import io
import os
import queue
import re
import shutil
import subprocess
import sys
import termios
import threading
import tkinter as tk

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
if getattr(sys, "frozen", False):
    # Running from a PyInstaller bundle: the native sim binary was added
    # alongside this script and unpacked next to it, not built into
    # build-sim/ next door -- there is no source tree here at all. Named
    # dash_sim_core rather than dash_sim: PyInstaller's frozen archive
    # already has an entry called "dash_sim" for this very module, and a
    # same-named embedded binary collides with it silently -- the app
    # just exits immediately with no error.
    SIM = os.path.join(sys._MEIPASS, "dash_sim_core")
else:
    SIM = os.path.join(os.path.dirname(HERE), "build-sim", "dash_sim")

# The ILI9488 module's active glass area, from its datasheet (320 RGB x 480,
# 玻璃显示区 48.96 x 73.44 mm) -- not the 480x320 the firmware renders at.
# The canvas is sized to this in real millimetres so the window shows the
# panel at the size it actually is on the bike, not at whatever a 1:1 pixel
# mapping happens to come out to on this monitor.
PANEL_W_MM = 73.44
PANEL_H_MM = 48.96

# 1.0 is true size: the window is the panel's real millimetres at the
# monitor's EDID-reported pixel density (7.5 px/mm on a 2560x1600 340 mm
# laptop screen, so the glass is 553x369 px). It was 1.5 for a while because
# the sim was tuned to look big rather than right. If a monitor's EDID lies,
# hold a ruler to the window and set DASH_SIM_PX_PER_MM, which bypasses this.
SIZE_CALIBRATION = 1.0


def _detect_px_per_mm(root):
    """Pixels per millimetre for the monitor actually showing the window.

    Tk's own winfo_screenmmwidth is the X server's screen-wide notion of
    physical size, which on a multi-monitor setup is the combined desktop,
    not any one panel -- dividing by it undersizes everything. xrandr
    reports each output's size from its EDID instead, so prefer that and
    match it to the monitor the window's top-left corner is actually on.
    Falls back to Tk's figure, then to 1:1 pixels, if xrandr is unavailable
    (e.g. Wayland) or its geometry can't be parsed. SIZE_CALIBRATION is
    applied on top of either -- see above.
    """
    override = os.environ.get("DASH_SIM_PX_PER_MM")
    if override:
        try:
            v = float(override)
            if v > 0:
                return v
        except ValueError:
            pass

    try:
        root.update_idletasks()
        win_x, win_y = root.winfo_rootx(), root.winfo_rooty()
        out = subprocess.run(["xrandr", "--current"], capture_output=True,
                             text=True, timeout=2).stdout
        best = None
        for line in out.splitlines():
            m = re.search(
                r"connected\s+(?:primary\s+)?(\d+)x(\d+)\+(\d+)\+(\d+).*?"
                r"(\d+)mm x (\d+)mm", line)
            if not m:
                continue
            pw, ph, ox, oy, mw, mh = (int(g) for g in m.groups())
            if mw <= 0:
                continue
            on_this_monitor = (ox <= win_x < ox + pw and oy <= win_y < oy + ph)
            rank = (on_this_monitor, "primary" in line)
            if best is None or rank > best[0]:
                best = (rank, pw / mw)
        if best:
            return best[1] * SIZE_CALIBRATION
    except (OSError, subprocess.SubprocessError):
        pass

    px_per_mm = root.winfo_fpixels("1m")
    if 0.5 <= px_per_mm <= 20:
        return px_per_mm * SIZE_CALIBRATION
    return 1.0 * SIZE_CALIBRATION

# Red end to end -- every lamp fitted to the PCB is red, so the window shows
# what the bar shows and the count of lit lamps is the whole reading.
LAMP_COLOURS = ["#ff4038"] * 8
LAMP_OFF = "#161a22"

BUTTONS = [
    ("A", "mode"),
    ("B", "screen"),
    ("C", "up"),
    ("D", "down"),
    ("E", "select"),
]

# Keyboard stand-ins for the pod, 1-5 left to right matching A-E above.
KEYS = {"1": "A", "2": "B", "3": "C", "4": "D", "5": "E"}

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

# The simulator has no radio, so nothing plays the phone's part of §11.1.
# Instead it uses whatever is actually
# playing on this machine, over MPRIS -- which is the useful half of a
# phone anyway, and gives the block real track names rather than ones
# chosen to fit it.
MEDIA_POLL_MS = 1000
# position and mpris:length are microseconds; length is blank for players
# that do not report one (most radio streams).
MEDIA_FORMAT = ("{{status}}\t{{position}}\t{{mpris:length}}"
                "\t{{title}}\t{{album}}\t{{artist}}")
MEDIA_STATE = {"Playing": 1, "Paused": 2}

def us_to_ms(field):
    """An MPRIS microsecond field to whole milliseconds, or None if blank."""
    try:
        return int(field) // 1000
    except ValueError:
        return None


COMMANDS = [
    ("lapline",   "Set finish line here"),
    ("laparm",    "Arm"),
    ("lapclear",  "Clear laps"),
    ("tripreset", "Reset trip A"),
    # Drops the accelerometer to zero and then spikes it, so the firmware's
    # own jump detector decides there was a jump. Nothing tells the dash.
    ("jump",      "Jump"),
]


# Which MCP23017 port-A pin each button is wired to: A B C D E.
# Keep in step with core::input::kPodPin.
POD_PINS = {"A": 3, "B": 5, "C": 7, "D": 4, "E": 6}


class PodSerial(threading.Thread):
    """Reads GPIOA lines from the pod's Arduino and queues (letter, down)."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.q = queue.Queue()
        self.fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0                      # raw
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = termios.B115200
        a[6][termios.VMIN], a[6][termios.VTIME] = 1, 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.mask = 0

    def run(self):
        buf = b""
        while True:
            try:
                chunk = os.read(self.fd, 256)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                m = re.match(rb"GPIOA=0x([0-9A-Fa-f]+)", line.strip())
                if not m:
                    continue
                v = int(m.group(1), 16)
                for letter, pin in POD_PINS.items():
                    down = bool(v & (1 << pin))
                    if down != bool(self.mask & (1 << pin)):
                        self.q.put((letter, down))
                self.mask = v


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
        """Returns (ppm_bytes, lamp_mask, flash, mode, screen, wire) or None."""
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
        # key=value tail, so the simulator can add a field without this
        # having to know about it.
        tail = dict(p.split("=", 1) for p in parts[5:] if "=" in p)

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
        return header + data, lamps, flash, mode, screen, tail

    def close(self):
        self.send("quit")
        try:
            self.p.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.p.kill()


class App:
    def __init__(self, root, sim, pod=None):
        self.sim = sim
        self.pod = pod
        self.root = root
        self.blink = False
        self.native_size = (0, 0)
        root.title("dash — simulator")
        root.configure(bg="#0d0f14")

        # Real pixels, not whatever the display's DPI scaling would hand
        # tkinter otherwise -- winfo_fpixels("1m") below needs to be reading
        # the screen's actual pixel density, not one Tk has already fudged.
        root.tk.call("tk", "scaling", 1.0)

        px_per_mm = _detect_px_per_mm(root)
        self.target_size = (round(PANEL_W_MM * px_per_mm),
                             round(PANEL_H_MM * px_per_mm))
        self._resize_to = self.target_size

        left = tk.Frame(root, bg="#0d0f14")
        left.grid(row=0, column=0, padx=12, pady=12, sticky="n")

        # --- shift lamps, above the panel as they are on the bar ---------
        # Flush with the panel's left edge rather than centred over it: the
        # strip is narrower than the panel, and a centred one reads as a
        # thing floating above the dash instead of a fixture on the bar.
        self.lamps = []
        strip = tk.Frame(left, bg="#0d0f14")
        strip.pack(anchor="w", pady=(0, 8))
        for i in range(8):
            lamp = tk.Canvas(strip, width=30, height=18, bg=LAMP_OFF,
                             highlightthickness=1,
                             highlightbackground="#2c3440")
            lamp.grid(row=0, column=i, padx=2)
            self.lamps.append(lamp)

        # --- the panel, with the button pod to its right ------------------
        # On the bike the pod is on the handlebar, not under the panel, so
        # the sim's buttons sit beside the screen rather than below it.
        panel_row = tk.Frame(left, bg="#0d0f14")
        panel_row.pack(anchor="w")

        tw, th = self.target_size
        self.canvas = tk.Canvas(panel_row, width=tw, height=th,
                                bg="black", highlightthickness=1,
                                highlightbackground="#2c3440")
        self.canvas.grid(row=0, column=0)
        self.photo = None
        self.image_id = self.canvas.create_image(0, 0, anchor="nw")

        # --- buttons -----------------------------------------------------
        pad = tk.Frame(panel_row, bg="#0d0f14")
        pad.grid(row=0, column=1, padx=(10, 0), sticky="n")
        self.button_widgets = {}
        for i, (key, what) in enumerate(BUTTONS):
            b = tk.Button(pad, text=f"{key}\n{what}", width=6, height=2,
                          bg="#161a22", fg="#e2e8f0",
                          activebackground="#38aaff",
                          highlightbackground="#2c3440")
            b.grid(row=i, column=0, pady=3)
            # Press and release are sent separately so the firmware's own
            # debounce and long-press timing decide what the press was.
            b.bind("<ButtonPress-1>", lambda e, k=key: self.sim.send(f"btn {k} down"))
            b.bind("<ButtonRelease-1>", lambda e, k=key: self.sim.send(f"btn {k} up"))
            self.button_widgets[key] = b
        tk.Label(pad, text="hold for a\nlong press, or 1-5",
                 fg="#788496", bg="#0d0f14", font=("TkDefaultFont", 8),
                 justify="center").grid(row=len(BUTTONS), column=0, pady=(6, 0))

        # --- 1-5 as a stand-in for the pod ---------------------------------
        # X11 auto-repeats a held key as KeyRelease/KeyPress pairs, not one
        # long press, so a naive binding would send down/up on every repeat.
        # A key held down only ever produces the outer down and the final
        # up: each release is deferred by one tick, and the matching repeat
        # press -- which always arrives before that tick runs -- cancels it
        # instead of sending a fresh down.
        self.keys_down = set()
        self.key_release_timers = {}
        root.bind_all("<KeyPress>", self.on_key_press)
        root.bind_all("<KeyRelease>", self.on_key_release)

        # Pinned to the panel row's own (fixed) width via a propagate-off
        # frame, not left to size itself to whatever the status text
        # happens to be this frame. A wraplength alone still lets a short
        # line's own natural width leak through as the request, which
        # rounds differently frame to frame and shows up as the sliders
        # jittering sideways by a pixel.
        panel_row.update_idletasks()
        status_box = tk.Frame(left, bg="#0d0f14",
                              width=panel_row.winfo_reqwidth(), height=34)
        status_box.pack_propagate(False)
        status_box.pack(fill="x", pady=(6, 0))
        self.status = tk.Label(status_box, text="", fg="#7884" "96",
                               bg="#0d0f14", font=("TkFixedFont", 9),
                               justify="left", anchor="nw",
                               wraplength=panel_row.winfo_reqwidth())
        self.status.pack(fill="both", expand=True)

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

        # --- what is playing, from this machine --------------------------
        self.media_var = tk.IntVar(value=1)
        self.media_last = None
        have_playerctl = shutil.which("playerctl") is not None
        cb = tk.Checkbutton(
            right,
            text="Music from this machine" if have_playerctl
            else "Music (needs playerctl)",
            variable=self.media_var, fg="#e2e8f0", bg="#0d0f14",
            selectcolor="#161a22", activebackground="#0d0f14",
            activeforeground="#e2e8f0", anchor="w",
            state="normal" if have_playerctl else "disabled",
            command=self.refresh_media)
        cb.pack(fill="x")
        if have_playerctl:
            self.poll_media()
        else:
            self.sim.send("media")

        tk.Label(right, text="", bg="#0d0f14").pack()
        for cmd, label in COMMANDS:
            tk.Button(right, text=label, bg="#161a22", fg="#e2e8f0",
                      highlightbackground="#2c3440",
                      command=lambda c=cmd: self.sim.send(f"cmd {c}")).pack(
                          fill="x", pady=2)

        root.protocol("WM_DELETE_WINDOW", self.quit)
        self.pump()

    def on_key_press(self, event):
        letter = KEYS.get(event.keysym)
        if letter is None:
            return
        timer = self.key_release_timers.pop(letter, None)
        if timer is not None:
            self.root.after_cancel(timer)
            return  # was an auto-repeat press, not a new press
        if letter in self.keys_down:
            return
        self.keys_down.add(letter)
        self.sim.send(f"btn {letter} down")
        b = self.button_widgets.get(letter)
        if b is not None:
            b.configure(bg=b["activebackground"])

    def on_key_release(self, event):
        letter = KEYS.get(event.keysym)
        if letter is None:
            return

        def do_release():
            self.key_release_timers.pop(letter, None)
            self.keys_down.discard(letter)
            self.sim.send(f"btn {letter} up")
            b = self.button_widgets.get(letter)
            if b is not None:
                b.configure(bg="#161a22")

        self.key_release_timers[letter] = self.root.after(1, do_release)

    def poll_media(self):
        """The timer. Kept separate from the refresh so that clicking the
        checkbox does not start a second one alongside the first."""
        self.refresh_media()
        self.root.after(MEDIA_POLL_MS, self.poll_media)

    def refresh_media(self):
        """Ask MPRIS what is playing and forward it to the sim, the way a
        phone would: the track when it changes, and the position as an
        anchor the dash extrapolates from."""
        line = ""
        position_ms = duration_ms = None
        state = 0
        if self.media_var.get():
            try:
                r = subprocess.run(
                    ["playerctl", "metadata", "--format", MEDIA_FORMAT],
                    capture_output=True, text=True, timeout=1)
                # Non-zero is the normal "no players found" case, not an
                # error worth reporting -- most of the time nothing is on.
                if r.returncode == 0:
                    f = r.stdout.strip("\n").split("\t", 3)
                    status = f[0]
                    # A stopped player still reports its last track. That is
                    # not what is playing, so it is not what the dash shows.
                    if status != "Stopped" and len(f) == 4:
                        line = f[3]
                        state = MEDIA_STATE.get(status, 1)
                        position_ms = us_to_ms(f[1])
                        duration_ms = us_to_ms(f[2])
            except (OSError, subprocess.SubprocessError):
                self.media_var.set(0)

        if line != self.media_last:
            self.media_last = line
            self.sim.send("media " + line if line else "media")
        if not line:
            return
        self.sim.send(f"set duration {duration_ms or 0}")
        self.sim.send(f"set playstate {state}")
        if position_ms is not None:
            self.sim.send(f"set position {position_ms}")

    def drain_pod(self):
        while self.pod is not None:
            try:
                letter, down = self.pod.q.get_nowait()
            except queue.Empty:
                return
            self.sim.send(f"btn {letter} {'down' if down else 'up'}")
            b = self.button_widgets.get(letter)
            if b is not None:
                b.configure(bg=b["activebackground"] if down else "#161a22")

    def pump(self):
        self.drain_pod()
        frame = self.sim.read_frame()
        if frame is None:
            self.status.config(text="simulator exited")
            return
        ppm, lamps, flash, mode, screen, tail = frame

        frame = Image.open(io.BytesIO(ppm))
        w, h = frame.size
        if (w, h) != self.native_size:
            self.native_size = (w, h)
            tw, th = self.target_size
            # The physical size is measured landscape; if the firmware
            # ever sends a portrait frame, swap it rather than stretch a
            # landscape panel's mm into a portrait box.
            if (w < h) != (tw < th):
                tw, th = th, tw
            self._resize_to = (tw, th)
            self.canvas.config(width=tw, height=th)
        frame = frame.resize(self._resize_to, Image.LANCZOS)
        # tk.PhotoImage over ImageTk: the latter needs the separate
        # python3-pil.imagetk package, and re-encoding to PPM is cheap at
        # this resolution.
        buf = io.BytesIO()
        frame.save(buf, format="PPM")
        self.photo = tk.PhotoImage(data=buf.getvalue())
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

        # What this frame would have cost the real panel. The laptop will
        # repaint anything at any rate; the bike will not, and 16.7 ms is
        # the whole of a 60 Hz frame. Amber past that, red past a full
        # repaint's worth (about 31 ms on the 8080 bus at 10 MHz).
        ms = float(tail.get("wire", 0.0))
        over = int(tail.get("over", 0))
        colour = "#788496"
        if ms > 16.7:
            colour = "#ffb020"
        if ms > 31.0 or over:
            colour = "#ff4038"
        text = (f"{mode} / {screen}   wire {ms:5.1f} ms"
                f"   {tail.get('px', '0')} px in {tail.get('tx', '0')} tx"
                f"   peak {float(tail.get('peak', 0.0)):.1f} ms")
        if over:
            text += f"   {over} transfers over the bus limit"
        self.status.config(text=text, fg=colour)
        self.root.after(1, self.pump)

    def quit(self):
        self.sim.close()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pod", metavar="PORT",
                    help="serial port of an Arduino reading the real button pod")
    args = ap.parse_args()
    pod = None
    if args.pod:
        pod = PodSerial(args.pod)
        pod.start()
    sim = Sim(SIM)
    root = tk.Tk()
    App(root, sim, pod)
    root.mainloop()


if __name__ == "__main__":
    main()
