#!/usr/bin/env python3
"""Fetch album covers and drop a cover.jpg in each album folder.

DashMedia shows embedded art if a file has any, and falls back to a cover file
sitting beside the tracks. Plenty of rips have neither -- The Way of All Flesh
has not a single embedded image in it -- which is what this fixes.

    python3 tools/fetch_covers.py --dry-run
    python3 tools/fetch_covers.py --album "The Way of All Flesh" --push

Art comes from the Cover Art Archive, found through MusicBrainz. Both are free
and unkeyed. MusicBrainz asks for no more than one request a second and a User-
Agent that identifies the caller, and this obeys both -- being rude to a free
service on someone else's behalf is not on.

Covers are downscaled before they are written. The archive routinely serves
3000x3000 JPEGs of 7 MB or more; the phone's screen is 240x320 and decoding
one of those for a thumbnail costs 36 MB of heap on a 2 GB handset.

What leaves this machine: the artist and album name of each folder.
"""

from __future__ import annotations

import argparse
import io
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

MB = "https://musicbrainz.org/ws/2"
CAA = "https://coverartarchive.org"
UA = "dashmedia-art/0.1 ( personal music library, one request per second )"

AUDIO = {".mp3", ".flac", ".m4a", ".ogg", ".opus", ".wma", ".wav"}
COVER = "cover.jpg"

# MusicBrainz's rate limit. Not negotiable, and not worth arguing with.
MB_INTERVAL = 1.1
_last_mb = 0.0


def mb_get(url: str, tries: int = 4):
    """A MusicBrainz call, paced and retried.

    503 is how MusicBrainz sheds load rather than an error in the request, so
    it is worth waiting out -- backing off rather than hammering, since the
    whole point of the rate limit is not to be the reason they shed load.
    """
    global _last_mb
    delay = 2.0
    for attempt in range(tries):
        wait = MB_INTERVAL - (time.monotonic() - _last_mb)
        if wait > 0:
            time.sleep(wait)
        _last_mb = time.monotonic()
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        try:
            with urllib.request.urlopen(req, timeout=25) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            if e.code in (503, 429) and attempt < tries - 1:
                time.sleep(delay)
                delay *= 2
                continue
            raise
    return {}


def fetch(url: str) -> bytes | None:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


# ---- the library -----------------------------------------------------------


def album_folders(root: Path) -> list[tuple[Path, str, str]]:
    """(folder, artist, album), one per directory that holds audio."""
    from mutagen import File

    out = []
    seen: set[Path] = set()
    for p in sorted(root.rglob("*")):
        if p.suffix.lower() not in AUDIO or not p.is_file():
            continue
        if p.parent in seen:
            continue
        seen.add(p.parent)
        try:
            m = File(p, easy=True)
        except Exception:
            continue
        if not m:
            continue
        first = lambda k: (m.get(k) or [""])[0].strip()
        artist, album = first("albumartist") or first("artist"), first("album")
        if artist and album:
            out.append((p.parent, artist, album))
    return out


# Rippers append their own notes to the album tag, and MusicBrainz knows the
# album by its actual name: "The Way Of All Flesh (vinyl)" finds nothing,
# "The Way Of All Flesh" finds it at once. Same for "(Japanese Edition)",
# "(10th Anniversary Edition 2018)" and a leading year.
QUALIFIER = re.compile(r"\s*[\(\[][^)\]]*[\)\]]")
LEADING_YEAR = re.compile(r"^\s*(19|20)\d{2}\s*[-–]\s*")


def clean_album(album: str) -> str:
    return LEADING_YEAR.sub("", QUALIFIER.sub("", album)).strip(" -–")


def search(artist: str, album: str) -> bytes | None:
    q = urllib.parse.quote(f'artist:"{artist}" AND releasegroup:"{album}"')
    data = mb_get(f"{MB}/release-group/?query={q}&fmt=json&limit=3")
    for g in data.get("release-groups") or []:
        img = fetch(f"{CAA}/release-group/{g['id']}/front")
        if img:
            return img
    return None


def find_cover(artist: str, album: str) -> bytes | None:
    # The tag as written first: if a release really is called "Vol. 3 (The
    # Subliminal Verses)" then stripping the brackets would find the wrong one.
    tried = []
    for name in (album, clean_album(album)):
        if not name or name in tried:
            continue
        tried.append(name)
        img = search(artist, name)
        if img:
            return img
    return None


def shrink(raw: bytes, size: int) -> bytes:
    from PIL import Image

    im = Image.open(io.BytesIO(raw))
    im = im.convert("RGB")
    im.thumbnail((size, size), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=88, optimize=True)
    return buf.getvalue()


# ---- the phone -------------------------------------------------------------


def phone_dirs() -> dict[tuple[str, str], str]:
    """(artist, album) -> the folder that album lives in on the phone."""
    cmd = [
        "adb", "shell", "content", "query",
        "--uri", "content://media/external/audio/media",
        "--projection", "_data:album:artist",
    ]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=180).stdout
    except Exception as e:
        print(f"  could not read the phone's library: {e}")
        return {}
    # See fetch_lyrics.phone_index: the output is unquoted, so split only where
    # a known column begins or every album with a comma in it is truncated.
    sep = re.compile(r", (?=(?:_data|album|artist)=)")
    index = {}
    for line in out.splitlines():
        if not line.startswith("Row:"):
            continue
        fields = {}
        for part in sep.split(re.sub(r"^Row: \d+ ", "", line)):
            k, _, v = part.partition("=")
            if k:
                fields[k.strip()] = v.strip()
        data, album, artist = fields.get("_data"), fields.get("album"), fields.get("artist")
        if data and album and artist:
            index[(artist.lower(), album.lower())] = str(Path(data).parent)
    return index


# ---- main ------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=Path.home() / "Music")
    ap.add_argument("--album", help="only albums whose name contains this")
    ap.add_argument("--size", type=int, default=500, help="max edge in pixels")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true", help="refetch existing covers")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--push", action="store_true", help="also copy to the phone")
    args = ap.parse_args()

    if not args.dir.is_dir():
        print(f"no such directory: {args.dir}")
        return 1

    albums = album_folders(args.dir)
    if args.album:
        needle = args.album.lower()
        albums = [a for a in albums if needle in a[2].lower()]
    print(f"{len(albums)} album folders\n")

    got, had, missed = 0, 0, []
    done = 0
    for folder, artist, album in albums:
        if args.limit and done >= args.limit:
            break
        target = folder / COVER
        if target.exists() and not args.force:
            had += 1
            continue
        done += 1
        try:
            raw = find_cover(artist, album)
        except KeyboardInterrupt:
            print("\ninterrupted")
            break
        except Exception as e:
            raw = None
            print(f"  !!   {artist} - {album}  ({e})")
        if not raw:
            missed.append((artist, album))
            print(f"  --   {artist} - {album}")
            continue
        small = shrink(raw, args.size)
        if not args.dry_run:
            target.write_bytes(small)
        got += 1
        print(f"  ok   {artist} - {album}   "
              f"({len(raw) // 1024} kB -> {len(small) // 1024} kB)")

    pushed = 0
    if args.push:
        print("\nreading the phone's library ...")
        index = phone_dirs()
        for folder, artist, album in albums:
            src = folder / COVER
            if not src.exists():
                continue
            remote = index.get((artist.lower(), album.lower()))
            if not remote:
                continue
            dest = f"{remote}/{COVER}"
            if args.dry_run:
                print(f"  would push -> {dest}")
            else:
                r = subprocess.run(["adb", "push", str(src), dest],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"  push failed for {album}: {r.stderr.strip()}")
                    continue
            pushed += 1
        print(f"pushed {pushed} covers")

    print("\n--- summary ---")
    print(f"fetched : {got}")
    print(f"already : {had}")
    print(f"missing : {len(missed)}")
    if args.dry_run:
        print("\n(dry run: nothing was written)")
    elif got and not args.push:
        print("\nNothing was copied to the phone. To send them:")
        print("    python3 tools/fetch_covers.py --push")
    return 0


if __name__ == "__main__":
    sys.exit(main())
