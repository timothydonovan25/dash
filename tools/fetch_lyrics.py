#!/usr/bin/env python3
"""Fetch time-synced lyrics and write a .lrc beside each track.

DashMedia reads sidecar .lrc files: `Song.lrc` next to `Song.mp3`. This makes
them, from lrclib.net, which is free and needs no key.

    python3 tools/fetch_lyrics.py --dry-run          # see what it would do
    python3 tools/fetch_lyrics.py                    # write the files
    python3 tools/fetch_lyrics.py --push             # and copy them to the phone

Only *synced* lyrics are written. Plain lyrics have no timestamps, every line
is dropped by the parser, and the app would report no lyrics at all -- so a
track that only has plain lyrics is counted as a miss rather than written.

That also applies to .lrc files already on disk: an untimed one is worthless
to the app, so by default it is refetched and the original kept as `.lrc.bak`.
`--keep-plain` leaves them alone instead.

What leaves this machine: the artist, title, album and duration of each track,
sent to lrclib.net. Nothing else, and only for tracks that need looking up.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

API = "https://lrclib.net/api"

# lrclib asks callers to identify themselves so it can tell traffic apart.
UA = "dashmedia-lrc/0.1 (https://github.com/; personal music library)"

AUDIO = {".mp3", ".flac", ".m4a", ".ogg", ".opus", ".wma", ".wav"}

# A line that starts with a timestamp is what makes a .lrc worth having.
TIMED = re.compile(r"^\[\d{1,3}:[0-5]\d", re.MULTILINE)


@dataclass
class Track:
    path: Path
    artist: str
    title: str
    album: str
    seconds: int

    @property
    def lrc(self) -> Path:
        return self.path.with_suffix(".lrc")


def is_synced(text: str) -> bool:
    return bool(TIMED.search(text))


# ---- reading the library ---------------------------------------------------


def read_tags(path: Path):
    """Artist, title, album and duration, or None if the file is unreadable."""
    from mutagen import File  # imported here so --help works without it

    try:
        m = File(path, easy=True)
    except Exception:
        return None
    if m is None:
        return None
    first = lambda k: (m.get(k) or [""])[0].strip()
    seconds = int(m.info.length) if m.info and m.info.length else 0
    return Track(path, first("artist"), first("title"), first("album"), seconds)


def scan(root: Path) -> list[Track]:
    out = []
    for p in sorted(root.rglob("*")):
        if p.suffix.lower() not in AUDIO or not p.is_file():
            continue
        t = read_tags(p)
        # Without an artist and a title there is nothing to search on; the
        # filename is too unreliable to guess from.
        if t and t.artist and t.title:
            out.append(t)
    return out


# ---- the lookup ------------------------------------------------------------


def get_json(url: str, timeout: int = 20):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None  # simply not in the database
        raise


def lookup(t: Track) -> tuple[str | None, str]:
    """Return (synced lyrics, reason). Lyrics is None when there are none."""
    q = urllib.parse.urlencode(
        {
            "artist_name": t.artist,
            "track_name": t.title,
            "album_name": t.album,
            "duration": t.seconds,
        }
    )
    hit = get_json(f"{API}/get?{q}")

    if hit is None:
        # The exact match is fussy about album spelling and duration. Searching
        # on artist and title alone catches remasters, live versions and the
        # dozen ways an album gets named by different rippers.
        q = urllib.parse.urlencode({"artist_name": t.artist, "track_name": t.title})
        results = get_json(f"{API}/search?{q}") or []
        synced = [r for r in results if r.get("syncedLyrics")]
        if not synced:
            return None, "not found"
        # Closest duration wins; a live version of a song is a different length
        # and its timings would be wrong for this file.
        hit = min(synced, key=lambda r: abs((r.get("duration") or 0) - t.seconds))
        if t.seconds and abs((hit.get("duration") or 0) - t.seconds) > 8:
            return None, "only a poor duration match"

    if hit.get("instrumental"):
        return None, "instrumental"
    lyrics = hit.get("syncedLyrics")
    if not lyrics:
        return None, "plain lyrics only"
    return lyrics, "ok"


# ---- pushing to the phone --------------------------------------------------


def phone_index() -> dict[tuple[str, str], str]:
    """(artist, title) -> path on the phone, read from its MediaStore.

    Matched through tags rather than paths because the two libraries are not
    laid out the same: `artists/Slipknot/Album/` here, `Music/Album/` there.
    """
    cmd = [
        "adb", "shell", "content", "query",
        "--uri", "content://media/external/audio/media",
        "--projection", "_data:title:artist",
    ]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=180).stdout
    except Exception as e:
        print(f"  could not read the phone's library: {e}")
        return {}
    # `content query` prints `key=value, key=value` and does not quote or
    # escape anything, so splitting on ", " truncates every title with a comma
    # in it -- "Vermilion, Pt. 2" becomes "Vermilion" and stops matching.
    # Split only where a known column name begins.
    sep = re.compile(r", (?=(?:_data|title|artist)=)")
    index = {}
    for line in out.splitlines():
        if not line.startswith("Row:"):
            continue
        # Drop the "Row: N " prefix before the first key.
        body = re.sub(r"^Row: \d+ ", "", line)
        fields = {}
        for part in sep.split(body):
            k, _, v = part.partition("=")
            if k:
                fields[k.strip()] = v.strip()
        data, title, artist = fields.get("_data"), fields.get("title"), fields.get("artist")
        if data and title and artist:
            index[(artist.lower(), title.lower())] = data
    return index


def push(tracks: list[Track], index: dict, dry: bool) -> tuple[int, int]:
    pushed = unmatched = 0
    for t in tracks:
        if not t.lrc.exists():
            continue
        remote = index.get((t.artist.lower(), t.title.lower()))
        if not remote:
            unmatched += 1
            continue
        target = re.sub(r"\.[^./]+$", ".lrc", remote)
        if dry:
            print(f"  would push -> {target}")
        else:
            r = subprocess.run(
                ["adb", "push", str(t.lrc), target], capture_output=True, text=True
            )
            if r.returncode != 0:
                print(f"  push failed for {t.title}: {r.stderr.strip()}")
                continue
        pushed += 1
    return pushed, unmatched


# ---- main ------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=Path.home() / "Music",
                    help="library root (default ~/Music)")
    ap.add_argument("--dry-run", action="store_true", help="report, write nothing")
    ap.add_argument("--force", action="store_true", help="refetch even synced .lrc files")
    ap.add_argument("--keep-plain", action="store_true",
                    help="leave existing untimed .lrc files alone")
    ap.add_argument("--limit", type=int, default=0, help="stop after N lookups")
    ap.add_argument("--delay", type=float, default=0.05, help="seconds between requests")
    ap.add_argument("--push", action="store_true", help="also copy results to the phone")
    ap.add_argument("--push-only", action="store_true",
                    help="skip fetching; only copy existing .lrc files to the phone")
    args = ap.parse_args()

    if not args.dir.is_dir():
        print(f"no such directory: {args.dir}")
        return 1

    print(f"scanning {args.dir} ...")
    tracks = scan(args.dir)
    print(f"{len(tracks)} tracks with usable tags\n")

    counts = {"written": 0, "skipped": 0, "replaced": 0}
    misses: list[tuple[Track, str]] = []

    if not args.push_only:
        looked_up = 0
        for t in tracks:
            if args.limit and looked_up >= args.limit:
                break

            if t.lrc.exists() and not args.force:
                existing = t.lrc.read_text(encoding="utf-8", errors="replace")
                if is_synced(existing):
                    counts["skipped"] += 1
                    continue
                if args.keep_plain:
                    counts["skipped"] += 1
                    continue
                # Untimed, so useless to the app. Refetch, but keep the words.

            looked_up += 1
            try:
                lyrics, why = lookup(t)
            except KeyboardInterrupt:
                print("\ninterrupted")
                break
            except Exception as e:
                lyrics, why = None, f"error: {e}"

            if lyrics:
                if not args.dry_run:
                    if t.lrc.exists():
                        backup = t.lrc.with_suffix(".lrc.bak")
                        t.lrc.replace(backup)
                        counts["replaced"] += 1
                    t.lrc.write_text(lyrics, encoding="utf-8")
                counts["written"] += 1
                print(f"  ok   {t.artist} - {t.title}")
            else:
                misses.append((t, why))
                print(f"  --   {t.artist} - {t.title}  ({why})")

            time.sleep(args.delay)

    if args.push or args.push_only:
        print("\nreading the phone's library ...")
        index = phone_index()
        if index:
            pushed, unmatched = push(tracks, index, args.dry_run)
            print(f"pushed {pushed}, {unmatched} tracks had no match on the phone")

    print("\n--- summary ---")
    print(f"written : {counts['written']}   (replacing {counts['replaced']} untimed files)")
    print(f"skipped : {counts['skipped']}   (already had synced lyrics)")
    print(f"missing : {len(misses)}")
    if misses:
        report = Path("lyrics-misses.txt")
        if not args.dry_run:
            report.write_text(
                "\n".join(f"{w:24} {t.artist} - {t.title}" for t, w in misses),
                encoding="utf-8",
            )
            print(f"          listed in {report}")
    if args.dry_run:
        print("\n(dry run: nothing was written)")
    elif not (args.push or args.push_only):
        # The failure this prevents: a full fetch that looks like a success,
        # while the phone still has nothing and the player reports no lyrics
        # for every track you try.
        on_disk = counts["written"] + counts["skipped"]
        if on_disk:
            print(f"\nNothing was copied to the phone. {on_disk} .lrc files are")
            print("on this machine only. To send them:")
            print("    python3 tools/fetch_lyrics.py --push-only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
