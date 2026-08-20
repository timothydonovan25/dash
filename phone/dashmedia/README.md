# DashMedia

The Android half of the dash's media path: a keypad-driven music player for a
Sonim XP3900, which also pushes what is playing to the dashboard over BLE.

The design, and the reasoning behind every awkward-looking decision in here,
is `docs/phone-media-app.md`. The wire protocol it implements is
`docs/system-architecture.md` §11.1, and that document wins.

## Build

**This project does not build on the machine it was written on** — there is no
Android SDK here, and no Gradle wrapper (the wrapper jar is a binary that has
to come from Gradle). Two ways forward:

- Open `phone/dashmedia/` in Android Studio. It provisions the SDK, generates
  the wrapper, and will offer to bump the versions in
  `gradle/libs.versions.toml`, none of which have ever been resolved.
- Or install `cmdline-tools`, set `ANDROID_HOME`, run `gradle wrapper` once,
  then `./gradlew assembleDebug`.

JDK 17 or 21. `targetSdk` is deliberately 30 to match the phone; see the doc.

## The fast loop

The protocol tests need no device and no emulator:

```
./gradlew :app:testDebugUnitTest
```

`Feed.kt` has no Android imports so that this stays true, for the same reason
`components/core/` has no ESP headers.

## Running it against something

`tools/dash_media_peripheral.py` in this repo is a laptop BLE peripheral that
advertises the §11.1 service and prints what the dash's CRUISE screen would
show. Use it: the dash's own `ble` task does not exist yet, and debugging two
unproven ends of one link is how a weekend disappears.

```bash
pip install bless
python3 tools/dash_media_peripheral.py
```

## Files

| File | What it is |
|---|---|
| `Feed.kt` | the wire format to the dash, and the decision of what is worth sending. Pure Kotlin, and the only file that knows the protocol |
| `DashLink.kt` | BLE central: scan, connect, MTU, and a serialised write queue |
| `MediaWatcher.kt` | the active `MediaSession` → `Snapshot`, so Spotify reaches the dash too |
| `NotifListener.kt` | empty, and required: it is the permission gate for the above |
| `PlayerService.kt` | ExoPlayer + `MediaSession`, the library cache, playback errors, and the owner of the link |
| `Library.kt` | MediaStore → tracks and read-only playlists |
| `Browse.kt` | grouping a flat list into artist → album → track |
| `Source.kt` | what the queue was built from, encoded so it survives a restart |
| `Store.kt` / `Stash.kt` | the playlists this app owns and the play counts: format, then filesystem |
| `Esc.kt` | delimiting user text in a flat string, safely. Used by both of the above |
| `T9.kt` | keypad search: 4-6-5 finds Gojira |
| `LeadingSilence.kt` + `LeadingSilenceProcessor.kt` | where a track actually starts, and the media3 processor that acts on it |
| `Art.kt` | album covers: embedded if the file has any, else a cover file in the folder |
| `Lrc.kt` | the .lrc format: timestamps, offsets, lookup by position. Pure, tested |
| `Lyrics.kt` | finding the sidecar next to a track, off the main thread, small cache |
| `Fmt.kt` | m:ss |
| `Keys.kt` | the XP3900 keypad, including the two softkeys the SDK has no names for |
| `Prefs.kt` | the little that outlives the process |
| `ui/` | browse, search, now playing, queue |

## Keys

The phone has no touchscreen, so the three softkeys under the screen are the
whole navigation scheme. They mean the same thing everywhere, because on a
keypad the only thing worse than too few controls is controls that move.

| Key | Everywhere |
|---|---|
| left softkey | settings |
| middle softkey | cycle: repeat the queue → repeat one track → shuffle |
| right softkey | swap between the library and what is playing |

| Screen | The rest |
|---|---|
| Browse | up/down the list, OK opens or plays, Back goes up a level. At the top of a section, **left/right switch ARTISTS and PLAYLISTS**. **2-9 starts a search**, `0` opens options for the highlighted row, `*` plays next, `#` appends to the queue |
| Search | 2-9 type, Back deletes a digit before it leaves, OK plays |
| Now playing | OK play/pause · left/right tap to skip, **hold to scrub** (8× for two seconds, then 32×) · **up opens the lyrics**, down the queue. The line being sung shows under the progress bar |
| Lyrics | follows the song; up/down reads ahead and it stops chasing until the next line |
| Queue | OK plays that entry · `*` move up · `#` move down · `0` remove |

Digits go to search rather than to actions, because search is what you reach
for most and a keypad phone has trained everyone to expect it.

Two of those softkeys are Sonim additions with no constant in `KeyEvent` —
`MULTIFUNC_CENTER` is 294 and `MULTIFUNC_RIGHT` is 295 on this handset, read
off the device rather than looked up. `Keys.kt` also matches on scan code, so
another keypad that reports the same physical key still works.

## The side buttons

The XP3900's two programmable keys do not send a keycode to anything. Settings
→ Programmable Keys binds each one to an **app**, which is why one of them
opens DashMedia and the other used to open the torch:

| Key | scan | Bound to |
|---|---|---|
| PTT key | 149 | DashMedia — opens the library |
| ALARM key | 148 | DashMedia's `Play/Pause` entry |

So play/pause on a side button means being an app that toggles playback when
launched. `PlayPauseActivity` is a second launcher entry with no interface
that does exactly that and finishes. It costs one extra icon in the app
drawer, which is the price of the mechanism rather than a choice, and the
Programmable Keys screen shows both keys as plain "DashMedia" because it
displays the package label rather than the activity's — the picker itself
lists them apart.

It dispatches a media key when any app holds a session, so the button governs
whatever is actually playing rather than only this app, and falls back to
waking our own player when nothing does — pressing it cold resumes the last
track instead of doing nothing.

## Playlists

Two kinds, and the list says which is which:

- **Ours** — created, renamed, reordered and deleted freely, held in app
  storage as one record per line and written through a temporary file, so a
  process killed mid-write loses the change rather than the file.
- **Theirs** — the ones already on the phone, read from MediaStore and
  **read-only**. An app on API 30 may only modify playlists it created, so
  editing one means taking an editable copy, which is an explicit action.

## Lyrics

Sidecar `.lrc` files only, read from beside the track — `Song.lrc` next to
`Song.mp3`, or the `Song.mp3.lrc` some taggers write. The app fetches nothing;
`tools/fetch_lyrics.py` makes the files on a desktop and can copy them over:

```bash
python3 tools/fetch_lyrics.py --dry-run     # see what it would do
python3 tools/fetch_lyrics.py --push        # write them, then copy to the phone
```

It writes **synced lyrics only**. Plain lyrics have no timestamps, every line
is dropped by the parser, and the screen would report no lyrics at all — so an
untimed `.lrc` already on disk is refetched, with the original kept as
`.lrc.bak`.

A `.lrc` is **not a media file**, so MediaStore will not hand one over no
matter what is granted through it. That is why the app asks for all-files
access, and why it reads the track's path directly. Without the grant the
lyrics screen says so rather than looking empty. To grant it without
navigating Settings on a keypad:

```bash
adb shell appops set --uid com.kao.dashmedia MANAGE_EXTERNAL_STORAGE allow
```

## Album art

Embedded art first, then a `cover.jpg` (or `folder.jpg`, `front.jpg`) beside
the tracks — plenty of rips have no embedded image at all. `tools/fetch_covers.py`
gets them from the Cover Art Archive and can copy them over:

```bash
python3 tools/fetch_covers.py --album "The Way of All Flesh" --push
python3 tools/fetch_covers.py --push        # the whole library
```

Covers are downscaled before writing. The archive serves 3000×3000 JPEGs of
7 MB and up; the screen is 240×320, and decoding one of those for a thumbnail
would cost 36 MB of heap on a 2 GB phone.

## Known rough edges

- **Audio boost** is a system `AudioEffect`, and any handset may refuse it. A
  failure disables the effect and leaves the music playing.
- **Trim silent starts** resets when the audio sink flushes, which happens on a
  seek and a format change. Two same-format tracks played gaplessly may not
  flush in between, in which case the second lead-in survives.
- **`MediaStore.Audio.Playlists`** is deprecated from API 31. Reading them
  works here only because the app targets 30. Our own playlists do not depend
  on it and will outlive it.
- **AGP 8.5 cannot run a Java compile step on JDK 21** — it fails transforming
  `core-for-system-modules.jar`. There is no Java source in the project today;
  the first one to appear will hit this. It is why the About dialog reads the
  version from the package manager instead of `BuildConfig`.
- **Not exercised on the device yet**: creating a playlist end to end, and the
  skip-on-error path. Both have unit tests underneath them; neither button has
  been pressed on real hardware.
- **The side buttons do nothing with the flip shut.** The keys fire and the
  kernel reports them, but no key in `gpio-keys.kl` carries the `WAKE` flag, so
  Android discards them while the device is asleep. Not fixable from an app; a
  headset media button works, because that reaches the session directly.
