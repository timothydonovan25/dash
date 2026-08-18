#!/usr/bin/env python3
"""A laptop standing in for the dash's BLE Media service.

The point of this is sequencing. The phone app and the dash's `ble` task are
both unwritten and unproven, and debugging two unproven ends of one link is
how you spend a weekend learning nothing. This advertises exactly what
`docs/system-architecture.md` §11.1 specifies, so the phone app has a working
counterpart from its first run, and the dash later has a known-good client.

It also enforces the parts of §11.1 that are easy to get wrong on the phone:
it truncates at 59 bytes the way the firmware does, it treats each write as a
whole line with no reassembly, and it zeroes the mirror on disconnect. What it
prints is what the CRUISE screen would show.

    pip install bless
    python3 tools/dash_media_peripheral.py

Needs BlueZ and an adapter that can advertise. Run it, then start the app on
the phone; the scan filters on the service UUID so nothing else will match.
"""

import asyncio
import logging
import sys

try:
    from bless import (  # type: ignore
        BlessServer,
        GATTAttributePermissions,
        GATTCharacteristicProperties,
    )
except ImportError:
    sys.exit("needs bless:  pip install bless")

SERVICE = "6e5f0001-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_FEED = "6e5f0002-b5a3-f393-e0a9-e50e24dcca9e"

# char[60] in core::state::Media, so 59 bytes plus the NUL.
MAX_VALUE = 59

TAGS = {"T": "title", "A": "artist", "L": "album", "S": "state"}
STATES = {"0": "stop", "1": "play", "2": "pause"}

logging.basicConfig(level=logging.INFO, format="%(message)s")
log = logging.getLogger("dash")


class Mirror:
    """What core::state::Media would hold, given what has arrived."""

    def __init__(self):
        self.clear()

    def clear(self):
        self.title = ""
        self.artist = ""
        self.album = ""
        self.state = "0"
        self.updated = 0

    def feed(self, raw: bytes) -> None:
        # One write is one line. No reassembly across writes: that is what
        # keeps a dropped write costing one field instead of the parser's
        # state, and the firmware side depends on it.
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            log.warning("  empty write, ignored")
            return
        # Tag, exactly one separating space, then the value. Stripping only
        # one space keeps a title that legitimately starts with whitespace
        # intact, and matches what the firmware parser has to do.
        tag, value = line[0], line[1:]
        if value.startswith(" "):
            value = value[1:]

        if tag not in TAGS:
            # §11.1: unknown tags are ignored rather than being an error, so a
            # tag can be added later without breaking an older phone app.
            log.info("  ignored unknown tag %r", tag)
            return

        encoded = value.encode("utf-8")
        if len(encoded) > MAX_VALUE:
            log.warning("  %s over %d bytes, firmware would truncate", tag, MAX_VALUE)
            encoded = encoded[:MAX_VALUE]
            value = encoded.decode("utf-8", errors="ignore")

        setattr(self, TAGS[tag], value)
        self.updated += 1

    def show(self) -> str:
        if not self.updated:
            return "  (dash shows nothing: updated_ms is 0)"
        return (
            f"  title  {self.title or '--'}\n"
            f"  artist {self.artist or '--'}\n"
            f"  album  {self.album or '--'}\n"
            f"  state  {STATES.get(self.state, '?')}"
        )


mirror = Mirror()


def write_request(characteristic, value, **kwargs):
    raw = bytes(value)
    log.info("<- %r", raw.decode("utf-8", errors="replace"))
    mirror.feed(raw)
    log.info("%s", mirror.show())


async def main():
    server = BlessServer(name="Dash")
    server.write_request_func = write_request

    await server.add_new_service(SERVICE)
    await server.add_new_characteristic(
        SERVICE,
        CHAR_FEED,
        GATTCharacteristicProperties.write
        | GATTCharacteristicProperties.write_without_response,
        None,
        GATTAttributePermissions.writeable,
    )

    await server.start()
    log.info("advertising %s -- waiting for the phone", SERVICE)

    connected = False
    try:
        while True:
            await asyncio.sleep(1)
            now = await server.is_connected()
            if now and not connected:
                log.info("== connected; expect all four fields ==")
            elif connected and not now:
                # §11.1: the dash publishes a zeroed Media on disconnect. A
                # dashboard still showing what was playing forty minutes ago is
                # worse than one showing nothing.
                mirror.clear()
                log.info("== disconnected; Media zeroed ==")
            connected = now
    except KeyboardInterrupt:
        pass
    finally:
        await server.stop()


if __name__ == "__main__":
    asyncio.run(main())
