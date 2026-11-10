#!/usr/bin/env python3
"""The handlebar pod from the laptop keyboard, over the board's USB serial.

    . ~/.espressif/v6.0.2/esp-idf/export.sh     # for pyserial
    python3 tools/pod_keys.py [-p /dev/ttyACMn]   # default: the first Espressif USB port

On a terminal that reports key releases (kitty, and others with the kitty
keyboard protocol), keys 1-5 ARE buttons A..E: held while held, several at
once, sent as `hold <mask>` lines. The firmware debounces and decodes them
like the pod's (a long press is holding the key), and Doom plays from them:
1 forward, 2 back, 3 left, 4 right, 5 fire/yes, 1+2 menu, 3+4 use.

Anywhere else, presses arrive already decoded (`btn X` / `long X`), the
firmware's debounce and hold timing are not exercised, and Doom cannot be
played:
1 2 3 4 5   short press of A..E
! @ # $ %   (shift+1..5) long press

--screen SECS also asks for the screen every SECS seconds and keeps it in
--shot (screen.png): the buttons and what they did, on one port
(tools/dash_screen.py, console `screen`).

Paused for flashing without quitting: SIGUSR1 lets go of the port, SIGUSR2
takes it back (the board's new port, unless -p), keys and log resume:
    kill -USR1 $(cat $XDG_RUNTIME_DIR/pod_keys.pid); python3 tools/flash_dev.py
    kill -USR2 $(cat $XDG_RUNTIME_DIR/pod_keys.pid)
(not pkill -f, which also signals, and kills, any shell whose command line names it)

q / Ctrl-C quits. Prints the board's log alongside. Opens the port without
resetting the board (RTS is lowered before DTR). `hold` is the bike's
(main/serial_buttons.cpp); the demo has its touch zones.
"""
import argparse
import os
import re
import select
import signal
import sys
import time
import termios
import threading
import tty

import serial
from serial.tools import list_ports

SHORT = "12345"
LONG = "!@#$%"      # shift+1..5 on a US layout

# The kitty keyboard protocol: 1 disambiguate + 2 report press/repeat/release
# + 8 every key as an escape code. Focus reports (1004) let a key held when
# the window loses focus go, since its release goes to another window.
KITTY_ON = "\x1b[>11u\x1b[?1004h"
KITTY_OFF = "\x1b[<u\x1b[?1004l"
# CSI key[:alts] [; mods[:event]] [; text] u, and focus in/out.
KEY = re.compile(rb"\x1b\[(\d+)(?::[\d:]*)?(?:;(\d*)(?::(\d+))?)?(?:;[\d:]*)?u|\x1b\[([IO])")
RESEND_S = 0.25     # while held; the firmware lets go after 1 s of silence


def kitty_supported(fd):
    """Ask for the protocol's flags, then for the device attributes every
    terminal answers: a reply to the first before the second is support."""
    os.write(sys.stdout.fileno(), b"\x1b[?u\x1b[c")
    got = b""
    end = time.monotonic() + 0.5
    while time.monotonic() < end and not re.search(rb"\x1b\[\?[\d;]*c", got):
        if select.select([fd], [], [], 0.05)[0]:
            got += os.read(fd, 256)
    return re.search(rb"\x1b\[\?\d+u", got) is not None


def held_keys(fd, tx):
    """Keys 1-5 as held state, `hold <mask>` on every change and every
    RESEND_S while any is down."""
    mask, sent, last = 0, None, 0.0
    buf = b""

    def send():
        nonlocal sent, last
        tx(f"hold {mask}\n")
        sent, last = mask, time.monotonic()

    while True:
        if select.select([fd], [], [], RESEND_S)[0]:
            buf += os.read(fd, 256)
        for m in KEY.finditer(buf):
            if m.group(4):                      # focus: out drops everything
                if m.group(4) == b"O":
                    mask = 0
            else:
                key = int(m.group(1))
                mods = int(m.group(2) or 1) - 1
                event = int(m.group(3) or 1)    # 1 press, 2 repeat, 3 release
                if event != 3 and (key == ord("q") or (key == ord("c") and mods & 4)):
                    return
                if ord("1") <= key <= ord("5"):
                    bit = 1 << (key - ord("1"))
                    mask = mask & ~bit if event == 3 else mask | bit
            # every change, so a tap read whole in one go is still a press
            if mask != sent:
                send()
        # keep only an unfinished sequence at the end
        cut = buf.rfind(b"\x1b")
        buf = buf[cut:] if cut >= 0 and not KEY.match(buf[cut:]) else b""
        if mask and time.monotonic() - last >= RESEND_S:
            send()


def find_port():
    # A board that resets while its port is held open comes back as ttyACM1, 2...
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    return "/dev/ttyACM0"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("--screen", type=float, default=0, help="seconds between screenshots; 0 for none")
    ap.add_argument("--shot", default="screen.png", help="where the screenshot goes")
    a = ap.parse_args()
    fixed = a.port
    a.port = a.port or find_port()

    s = serial.Serial()
    s.baudrate, s.timeout = 115200, 0.2

    def open_port():
        s.port = a.port
        # The USB-Serial-JTAG resets the chip on DTR low with RTS high. Opening
        # with both False reset it every time: pyserial lowers DTR before RTS.
        # Open as the kernel does anyway (both high), then RTS down first.
        s.dtr = s.rts = True
        s.open()
        s.rts = False
        s.dtr = False

    open_port()
    paused = threading.Event()

    def tx(line):
        if paused.is_set():
            return                  # keys pressed while paused go nowhere
        try:
            s.write(line.encode())
        except (serial.SerialException, OSError, TypeError):
            if not paused.is_set():     # closed under us by pause() is fine
                raise

    def pause(*_):
        if paused.is_set():
            return
        try:
            s.write(b"hold 0\n")       # nothing stays held while away
        except (serial.SerialException, OSError):
            pass
        paused.set()                # before close, so the threads see why it failed
        s.close()
        sys.stdout.write(f"\r\n[pod_keys: paused, {a.port} free]\r\n")
        sys.stdout.flush()

    def resume(*_):
        if not paused.is_set():
            return
        end = time.monotonic() + 30     # the board is still enumerating after a flash
        while True:
            a.port = fixed or find_port()
            try:
                open_port()
                break
            except serial.SerialException:
                if time.monotonic() > end:
                    sys.stdout.write(f"\r\n[pod_keys: {a.port} not back, still paused; SIGUSR2 again]\r\n")
                    sys.stdout.flush()
                    return
                time.sleep(0.5)
        paused.clear()
        sys.stdout.write(f"\r\n[pod_keys: back on {a.port}]\r\n")
        sys.stdout.flush()

    signal.signal(signal.SIGUSR1, pause)
    signal.signal(signal.SIGUSR2, resume)
    pidfile = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "pod_keys.pid")
    with open(pidfile, "w") as f:
        f.write(f"{os.getpid()}\n")

    scr = None
    if a.screen:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from dash_screen import Screen
        scr = Screen()

    def echo():
        buf = b""
        while True:
            if paused.is_set():
                time.sleep(0.1)
                continue
            try:
                d = s.read(4096)
            except (serial.SerialException, OSError, TypeError):
                if paused.is_set():
                    continue
                return
            if not d:
                continue
            if scr is None:
                # the terminal is raw: give each \n its \r back
                sys.stdout.write(d.decode("utf-8", "replace").replace("\r", "").replace("\n", "\r\n"))
                sys.stdout.flush()
                continue
            # Line by line, so the screen's lines go to the decoder, not the terminal.
            buf += d
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", "replace").rstrip("\r")
                before = scr.frames
                if scr.feed(line):
                    if scr.frames != before:
                        scr.save(a.shot)
                    continue
                sys.stdout.write(line + "\r\n")
            sys.stdout.flush()

    def ask():
        while True:
            try:
                tx("screen\n")
            except (serial.SerialException, OSError, TypeError):
                return
            time.sleep(a.screen)

    threading.Thread(target=echo, daemon=True).start()
    if scr is not None:
        threading.Thread(target=ask, daemon=True).start()
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    try:
        if kitty_supported(fd):
            print(f"{a.port}: 1-5 are A-E, held as long as you hold them; q quits\r")
            sys.stdout.write(KITTY_ON)
            sys.stdout.flush()
            try:
                held_keys(fd, tx)
            finally:
                # The terminal first: a port gone mid-session raises on the
                # write, and left the shell typing escape codes.
                sys.stdout.write(KITTY_OFF)
                sys.stdout.flush()
                try:
                    tx("hold 0\n")
                except (serial.SerialException, OSError):
                    pass
            return
        print(f"{a.port}: 1-5 short, shift+1-5 long, q quits\r")
        while True:
            k = sys.stdin.read(1)
            if not k or k in ("q", "\x03", "\x04"):   # "" is EOF, and "" in SHORT is True
                break
            if k in SHORT:
                tx(f"btn {'ABCDE'[SHORT.index(k)]}\n")
            elif k in LONG:
                tx(f"long {'ABCDE'[LONG.index(k)]}\n")
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        s.close()
        try:
            os.remove(pidfile)
        except OSError:
            pass
        print()


if __name__ == "__main__":
    main()
