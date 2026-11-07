#!/usr/bin/env python3
"""What the dash's screen shows, over its USB serial (console `screen`).

    . ~/.espressif/v6.0.2/esp-idf/export.sh     # for pyserial
    python3 tools/dash_screen.py [-p /dev/ttyACMn] [-o screen.png]        # one shot
    python3 tools/dash_screen.py --live 1 [-o screen.png]                 # rewrite it every second

The firmware sends its copy of the glass (ui::Shadow) as text lines, one per
row, run-length coded (main/serial_buttons.cpp):

    SCREEN BEGIN <w> <h> <seq>
    SCR <y> <base64 of (u16 count, u16 rgb565) runs, little-endian>
    SCREEN END <seq>

A log line from another task can land between two rows; a row lost that way
keeps the last frame's. Standard library and pyserial only. tools/pod_keys.py
--screen uses the same decoder, for buttons and the screen on one port.
"""
import argparse
import base64
import struct
import sys
import time
import zlib


class Screen:
    """Feed it lines; it keeps the last whole frame as RGB rows."""

    def __init__(self):
        self.w = self.h = 0
        self.rows = []
        self.frames = 0
        self._open = False

    def feed(self, line):
        """True if `line` was screen data (so not log text)."""
        if line.startswith("SCREEN BEGIN "):
            _, _, w, h, _ = line.split()
            w, h = int(w), int(h)
            if (w, h) != (self.w, self.h):
                self.w, self.h = w, h
                self.rows = [bytes(w * 3)] * h
            self._open = True
            return True
        if line.startswith("SCREEN END") or line.startswith("SCREEN NONE"):
            if self._open:
                self.frames += 1
            self._open = False
            return True
        if line.startswith("SCR "):
            try:
                _, y, data = line.split(" ", 2)
                self.rows[int(y)] = self._row(base64.b64decode(data.strip()))
            except (ValueError, IndexError):
                pass        # a damaged row: the last frame's stays
            return True
        return False

    def _row(self, rle):
        out = bytearray()
        for i in range(0, len(rle) - 3, 4):
            n, c = struct.unpack_from("<HH", rle, i)
            r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
            out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * n
        return bytes(out[: self.w * 3].ljust(self.w * 3, b"\0"))

    def png(self):
        raw = b"".join(b"\0" + r for r in self.rows)

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

    def save(self, path):
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(self.png())
        import os
        os.replace(tmp, path)          # a viewer never sees half a file


def open_port(port):
    """As pod_keys.py does: without resetting the board."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = True
    s.open()
    s.rts = False
    s.dtr = False
    return s


def find_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    return "/dev/ttyACM0"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("-o", "--out", default="screen.png")
    ap.add_argument("--live", type=float, default=0, help="seconds between frames; 0 for one")
    a = ap.parse_args()
    s = open_port(a.port or find_port())
    scr = Screen()
    buf = b""
    try:
        while True:
            s.write(b"screen\n")
            want = scr.frames + 1
            t0 = time.time()
            asked = t0
            while scr.frames < want and time.time() - t0 < 10:
                if not scr._open and time.time() - asked > 2:
                    s.write(b"screen\n")   # a request sent as the port opens can be lost
                    asked = time.time()
                buf += s.read(65536)
                *lines, buf = buf.split(b"\n")
                for l in lines:
                    scr.feed(l.decode("ascii", "replace").rstrip("\r"))
            if scr.frames < want:
                sys.exit("no frame from the dash (an old firmware, or not the dash's port?)")
            scr.save(a.out)
            print(f"{a.out}: frame {scr.frames}, {time.time() - t0:.2f} s", file=sys.stderr)
            if not a.live:
                break
            time.sleep(a.live)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


if __name__ == "__main__":
    main()
