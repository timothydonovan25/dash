#!/usr/bin/env python3
"""Flash a build into the dash's dev slot over USB, leaving stable alone.

`idf.py flash` writes ota_0 and erases otadata, which makes whatever it wrote
the new stable: the recovery path. This is the other one. It reads otadata
off the dash, works out which slot is stable the way the bootloader would,
and writes the image into the *other* slot without touching otadata, so the
dash still boots stable. Then DIAG -> Update -> Boot dev once runs it
(system-architecture.md section 11.7).

    . ~/.espressif/v6.0.2/esp-idf/export.sh
    idf.py build
    python3 tools/flash_dev.py [-p /dev/ttyACM0] [build/dash.bin]

Refuses a file that is not a dash image for the ESP32-P4 -- the same checks
as core/ota's check_header(), so the C6's network_adapter.bin on the wrong
command line does not end up in an app slot -- and an image built for the
other board (`bike` vs `demo`, the second word of the version) than the one
in the stable slot, as the card and Wi-Fi updates refuse it. A bike image
on the demo board boots to a dark screen.

"""
import argparse
import binascii
import os
import struct
import subprocess
import sys
import tempfile

OTADATA_SECTOR = 0x1000          # each of the two otadata copies
STATE_OFFSET = 24                # esp_ota_select_entry_t: seq, label[20], state, crc
CRC_OFFSET = 28
VALID, UNDEFINED = 0x2, 0xFFFFFFFF
STATE_NAMES = {0: "new", 1: "pending verify", 2: "valid", 3: "invalid",
               4: "aborted", 0xFFFFFFFF: "undefined"}
SLOTS = ("ota_0", "ota_1")


def slot_offsets():
    """ota_0 and ota_1's offsets, from partitions.csv (both boards share it),
    so a moved table cannot leave this reading the wrong place."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "partitions.csv")
    found = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 4 and cols[0] in SLOTS:
                found[cols[0]] = int(cols[3], 0)
    if set(found) != set(SLOTS):
        sys.exit(f"{path}: no offsets for {', '.join(sorted(set(SLOTS) - set(found)))}")
    return tuple(found[s] for s in SLOTS)

IMAGE_MAGIC = 0xE9
CHIP_ESP32P4 = 0x0012
APP_DESC_MAGIC = 0xABCD5432
APP_DESC = 32                     # esp_image_header_t (24) + segment header (8)


def idf_tool(*parts):
    root = os.environ.get("IDF_PATH")
    if not root:
        sys.exit("IDF_PATH is not set: source ~/.espressif/v6.0.2/esp-idf/export.sh first")
    return os.path.join(root, *parts)


def check_image(path):
    """core::ota::check_header(), in Python. Returns the version string."""
    with open(path, "rb") as f:
        head = f.read(APP_DESC + 256)
    return check_head(head, path)


def check_head(head, path):
    if len(head) < APP_DESC + 256:
        sys.exit(f"{path}: too short to be an app image")
    if head[0] != IMAGE_MAGIC or head[1] == 0:
        sys.exit(f"{path}: not an ESP app image")
    chip = struct.unpack_from("<H", head, 12)[0]
    if chip != CHIP_ESP32P4:
        sys.exit(f"{path}: built for chip id {chip:#06x}, not the ESP32-P4")
    if struct.unpack_from("<I", head, APP_DESC)[0] != APP_DESC_MAGIC:
        sys.exit(f"{path}: an image, but not an application")
    project = head[APP_DESC + 48:APP_DESC + 80].split(b"\0")[0].decode(errors="replace")
    if project != "dash":
        sys.exit(f"{path}: project '{project}', not 'dash'")
    return head[APP_DESC + 16:APP_DESC + 48].split(b"\0")[0].decode(errors="replace")


def board_of(version):
    """"1.16 bike event driven logic" -> "bike" (cmake/dash_version.cmake)."""
    words = version.split()
    return words[1] if len(words) > 1 else None


def read_slot_version(port, slot):
    """The version string of the image in `slot`, or None if it holds none."""
    with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
        # --after no-reset: the write that follows connects again anyway, and
        # every reset from esptool risks the demo board's ROM boot wedge.
        subprocess.run([sys.executable, "-m", "esptool", "--port", port,
                        "--after", "no-reset", "read-flash", hex(slot_offsets()[slot]), hex(APP_DESC + 256),
                        tmp.name], check=True, stdout=subprocess.DEVNULL)
        head = open(tmp.name, "rb").read()
    try:
        return check_head(head, SLOTS[slot])
    except SystemExit:
        return None


def read_otadata(port):
    with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
        subprocess.run([sys.executable, idf_tool("components", "partition_table", "parttool.py"),
                        "--port", port, "--esptool-args=after=no-reset", "read_partition", "--partition-name", "otadata",
                        "--output", tmp.name], check=True)
        return open(tmp.name, "rb").read()


def stable_slot(otadata):
    """The slot the bootloader falls back to: the newest entry with a valid
    CRC whose state is valid or undefined. An entry that is new, pending or
    aborted is a dev boot, not stable. No such entry at all means otadata was
    erased by a USB flash, which boots ota_0."""
    best = None
    for i in range(2):
        e = otadata[i * OTADATA_SECTOR:i * OTADATA_SECTOR + 32]
        seq, state, crc = (struct.unpack_from("<I", e, 0)[0],
                           struct.unpack_from("<I", e, STATE_OFFSET)[0],
                           struct.unpack_from("<I", e, CRC_OFFSET)[0])
        if seq == 0xFFFFFFFF or crc != binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF):
            continue
        slot = (seq - 1) % len(SLOTS)
        print(f"  otadata[{i}]: seq {seq}, {SLOTS[slot]}, {STATE_NAMES.get(state, hex(state))}")
        if state in (VALID, UNDEFINED) and (best is None or seq > best[0]):
            best = (seq, slot)
    return best[1] if best else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("image", nargs="?", default="build/dash.bin")
    ap.add_argument("--any-board", action="store_true",
                    help="write the image even if it is for the other board")
    args = ap.parse_args()

    version = check_image(args.image)
    print(f"{args.image}: dash {version}")
    print("reading otadata from the dash")
    stable = stable_slot(read_otadata(args.port))
    dev = 1 - stable
    running = read_slot_version(args.port, stable)
    print(f"stable {SLOTS[stable]}: {running or 'no dash image'}")
    if running and board_of(running) != board_of(version) and not args.any_board:
        sys.exit(f"refusing: this is a {board_of(version)} image and the dash runs "
                 f"{board_of(running)} firmware (--any-board to write it anyway)")
    print(f"stable is {SLOTS[stable]}; writing {SLOTS[dev]}, otadata untouched")
    # parttool, not otatool: IDF v6.0.2's otatool write_ota_partition passes
    # its file as `input=` to a function that takes `input_file`, and dies
    # with a TypeError before writing anything. Same write, by slot name.
    subprocess.run([sys.executable, idf_tool("components", "partition_table", "parttool.py"),
                    # A USB reset after the write can wedge a P4 v1.x in its ROM
                    # until it is unplugged; the RTC watchdog reset is deeper.
                    "--port", args.port, "--esptool-args=after=watchdog-reset",
                    "write_partition", "--partition-name", SLOTS[dev],
                    "--input", args.image], check=True)
    print(f"done: {SLOTS[dev]} holds {version}. The dash still boots stable; "
          "DIAG -> Update -> Boot dev once to run it.")


if __name__ == "__main__":
    main()
