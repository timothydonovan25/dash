#!/usr/bin/env python3
"""The handlebar pod from the laptop keyboard, over the board's USB serial.

    . ~/.espressif/v6.0.2/esp-idf/export.sh     # for pyserial
    python3 tools/pod_keys.py [-p /dev/ttyACM0]

1 2 3 4 5   short press of A..E
! @ # $ %   (shift+1..5) long press
q / Ctrl-C  quit

Sends `btn X` / `long X` lines to main/serial_buttons.cpp (the bike) or
demo_p4/main/serial_buttons.cpp (the demo), and prints the board's log
alongside. Presses arrive already decoded, so the firmware's debounce and
hold timing are not exercised, and a held button does not repeat.
Opens the port with DTR/RTS low so it does not reset the board.
"""
import argparse
import sys
import termios
import threading
import tty

import serial

SHORT = "12345"
LONG = "!@#$%"      # shift+1..5 on a US layout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    a = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()

    def echo():
        while s.is_open:
            try:
                d = s.read(4096)
            except (serial.SerialException, TypeError):
                return
            if d:
                # the terminal is raw: give each \n its \r back
                sys.stdout.write(d.decode("utf-8", "replace").replace("\r", "").replace("\n", "\r\n"))
                sys.stdout.flush()

    threading.Thread(target=echo, daemon=True).start()
    print(f"{a.port}: 1-5 short, shift+1-5 long, q quits\r")

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    try:
        while True:
            k = sys.stdin.read(1)
            if not k or k in ("q", "\x03", "\x04"):   # "" is EOF, and "" in SHORT is True
                break
            if k in SHORT:
                s.write(f"btn {'ABCDE'[SHORT.index(k)]}\n".encode())
            elif k in LONG:
                s.write(f"long {'ABCDE'[LONG.index(k)]}\n".encode())
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        s.close()
        print()


if __name__ == "__main__":
    main()
