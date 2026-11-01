#!/usr/bin/env python3
"""The bike board's C6 over its own UART, through an FTDI TTL-232R-3V3.

bringup.md section 9's recovery flashing, as commands. The C6's factory
firmware does not speak ESP-Hosted, so slave_fw cannot reach it over SDIO;
this puts ESP-Hosted on it once, after which `idf.py c6-flash` takes over.

Wiring: black GND, yellow (cable RXD) to H3 (C6 TX), orange (cable TXD) to
H1 (C6 RX). Red is 5 V: leave it off. The cable's RTS/CTS go nowhere, so
nothing here can reset the C6: download mode is C6_BOOT/IO9 (R1's far end)
to GND through 1 kOhm, then tap C6_RST (R22's far end) to GND; the C6's own
watchdog resets do not re-read IO9. Park the P4 first (bringup.md section 9,
step 5), or its resets of the C6 cut a backup short.

    . ~/.espressif/v6.0.2/esp-idf/export.sh
    python3 tools/c6_uart.py listen          # boot text, saved to c6_boot.log
    python3 tools/c6_uart.py wait            # until the ROM bootloader answers
    python3 tools/c6_uart.py backup          # 4 MB to c6_factory_backup.bin
    python3 tools/c6_uart.py flash           # demo_p4/c6_cp's build
    python3 tools/c6_uart.py inspect         # what the backup holds; no board needed

Do not open the P4's /dev/ttyACM0 meanwhile: that resets the P4, which
resets the C6.
"""
import argparse
import os
import struct
import subprocess
import sys
import time

from flash_dev import APP_DESC, APP_DESC_MAGIC, IMAGE_MAGIC, OTADATA_SECTOR, stable_slot

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FLASH_SIZE = 0x400000                    # ESP32-C6-MINI-1U-N4
C6_BUILD = os.path.join(ROOT, "demo_p4", "c6_cp", "build")


def esptool(port, *args, baud=460800, check=True):
    # no-reset both ways: the cable has no line to EN or IO9
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "-p", port, "-b", str(baud),
           "--before", "no-reset", "--after", "no-reset", *args]
    return subprocess.run(cmd, check=check, cwd=C6_BUILD if args[0] == "write-flash" else None)


def listen(a):
    import serial                        # pyserial, in the ESP-IDF env
    with serial.Serial(a.port, 115200, timeout=0.2) as s, open(a.log, "ab") as log:
        s.dtr = s.rts = False            # unwired, but keep them idle
        print(f"listening on {a.port}, saving to {a.log}; power the board now, Ctrl-C to stop",
              file=sys.stderr)
        try:
            while True:
                data = s.read(4096)
                if data:
                    log.write(data)
                    log.flush()
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass


def wait(a):
    deadline = time.monotonic() + a.timeout
    while time.monotonic() < deadline:
        # the ROM answers at 115200 before any baud change
        if esptool(a.port, "chip-id", baud=115200, check=False).returncode == 0:
            print("C6 is in its ROM bootloader")
            return
        time.sleep(1)
    sys.exit("no answer: is IO9 grounded through 1 kOhm, H1/H3 the right way round, "
             "and GPIO8 high? (bringup.md section 9, step 4)")


def backup(a):
    if os.path.exists(a.out):
        sys.exit(f"{a.out} exists; the factory image is only on the C6 once, move it aside first")
    tmp = a.out + ".part"
    esptool(a.port, "read-flash", "0", hex(FLASH_SIZE), tmp)
    if os.path.getsize(tmp) != FLASH_SIZE:
        sys.exit(f"{tmp}: {os.path.getsize(tmp)} bytes, expected {FLASH_SIZE}")
    os.rename(tmp, a.out)
    print(f"saved {a.out}")


def flash(a):
    if not a.no_backup and (not os.path.exists(a.backup) or os.path.getsize(a.backup) != FLASH_SIZE):
        sys.exit(f"no 4 MB backup at {a.backup}: run `backup` first (or --no-backup)")
    if not os.path.exists(os.path.join(C6_BUILD, "flash_args")):
        sys.exit("demo_p4/c6_cp is not built: idf.py -C demo_p4/c6_cp set-target esp32c6 build")
    esptool(a.port, "write-flash", "@flash_args")
    print("done: unclip IO9, power-cycle, then `idf.py -p /dev/ttyACM0 c6-flash` (bringup.md step 8)")


CHIP_ESP32C6 = 0x000D
BOOT_DESC_MAGIC = 0x50                   # esp_bootloader_desc_t, IDF 5.2 on
PART_ENTRY = struct.Struct("<2sBBII16sI")
# strings that say what an image is for when its app description does not
MARKERS = (b"ESP-Hosted", b"sdio_slave", b"spi_slave", b"spi_hd", b"AT version")


def cstr(b):
    return b.split(b"\0")[0].decode(errors="replace")


def describe_app(img, off):
    head = img[off:off + APP_DESC + 256]
    if head.count(0xFF) == len(head):
        return "erased"
    if head[0] != IMAGE_MAGIC:
        return f"no image (first byte {head[0]:#04x})"
    chip = struct.unpack_from("<H", head, 12)[0]
    note = "" if chip == CHIP_ESP32C6 else f", chip id {chip:#06x}, not a C6"
    d = APP_DESC
    if head[d] == BOOT_DESC_MAGIC:
        return f"bootloader, IDF {cstr(head[d+8:d+40])}, built {cstr(head[d+40:d+64])}{note}"
    if struct.unpack_from("<I", head, d)[0] != APP_DESC_MAGIC:
        return f"image with no description{note}"
    # an ESP-Hosted network_adapter's version is the ESP-Hosted version
    return (f"'{cstr(head[d+48:d+80])}' version '{cstr(head[d+16:d+48])}', IDF {cstr(head[d+112:d+144])}, "
            f"built {cstr(head[d+96:d+112])} {cstr(head[d+80:d+96])}{note}")


def inspect(a):
    img = open(a.image, "rb").read()
    print(f"{a.image}: {len(img)} bytes" + ("" if len(img) == FLASH_SIZE else f", not {FLASH_SIZE}"))
    if img.count(0xFF) == len(img):
        print("the whole flash is erased: nothing was ever on it")
        return
    print(f"  0x0      {describe_app(img, 0)}")
    table = next((o for o in range(0x1000, 0x10001, 0x1000) if img[o:o + 2] == b"\xaa\x50"), None)
    if table is None:
        print("  no partition table between 0x1000 and 0x10000")
    else:
        print(f"  partition table at {table:#x}:")
        for i in range(0, 0xC00, PART_ENTRY.size):
            magic, kind, sub, off, size, label, _ = PART_ENTRY.unpack_from(img, table + i)
            if magic != b"\xaa\x50":
                break
            line = f"    {cstr(label):<16} {kind:#04x}/{sub:#04x} {off:#09x} {size // 1024:>5} kB"
            if kind == 0:
                line += "  " + describe_app(img, off)
            print(line)
            if kind == 1 and sub == 0:   # otadata: which app slot the bootloader picks
                otadata = img[off:off + 2 * OTADATA_SECTOR]
                if otadata.count(0xFF) == len(otadata):
                    print("    otadata erased: the bootloader runs factory, else ota_0")
                else:
                    stable_slot(otadata)
    found = [m.decode() for m in MARKERS if m in img]
    print("  strings: " + (", ".join(found) if found else "none of " + ", ".join(m.decode() for m in MARKERS)))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("-p", "--port", default="/dev/ttyUSB0")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("listen", help="print and save the C6's console")
    s.add_argument("--log", default="c6_boot.log")
    s.set_defaults(fn=listen)
    s = sub.add_parser("wait", help="poll until the C6's ROM bootloader answers")
    s.add_argument("--timeout", type=float, default=60)
    s.set_defaults(fn=wait)
    s = sub.add_parser("backup", help="read the C6's whole flash")
    s.add_argument("--out", default="c6_factory_backup.bin")
    s.set_defaults(fn=backup)
    s = sub.add_parser("flash", help="write demo_p4/c6_cp's ESP-Hosted build")
    s.add_argument("--backup", default="c6_factory_backup.bin")
    s.add_argument("--no-backup", action="store_true")
    s.set_defaults(fn=flash)
    s = sub.add_parser("inspect", help="say what a flash backup holds, offline")
    s.add_argument("image", nargs="?", default="c6_factory_backup.bin")
    s.set_defaults(fn=inspect)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
