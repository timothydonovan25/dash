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
command line does not end up in an app slot.
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


def read_otadata(port):
    with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
        subprocess.run([sys.executable, idf_tool("components", "partition_table", "parttool.py"),
                        "--port", port, "read_partition", "--partition-name", "otadata",
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
    args = ap.parse_args()

    version = check_image(args.image)
    print(f"{args.image}: dash {version}")
    print("reading otadata from the dash")
    stable = stable_slot(read_otadata(args.port))
    dev = 1 - stable
    print(f"stable is {SLOTS[stable]}; writing {SLOTS[dev]}, otadata untouched")
    # parttool, not otatool: IDF v6.0.2's otatool write_ota_partition passes
    # its file as `input=` to a function that takes `input_file`, and dies
    # with a TypeError before writing anything. Same write, by slot name.
    subprocess.run([sys.executable, idf_tool("components", "partition_table", "parttool.py"),
                    "--port", args.port, "write_partition", "--partition-name", SLOTS[dev],
                    "--input", args.image], check=True)
    print(f"done: {SLOTS[dev]} holds {version}. The dash still boots stable; "
          "DIAG -> Update -> Boot dev once to run it.")


if __name__ == "__main__":
    main()
