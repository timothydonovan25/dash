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
| `Feed.kt` | the wire format and the decision of what is worth sending. Pure Kotlin, and the only file that knows the protocol |
| `DashLink.kt` | BLE central: scan, connect, MTU, and a serialised write queue |
| `MediaWatcher.kt` | the active `MediaSession` → `Snapshot`, so Spotify reaches the dash too |
| `NotifListener.kt` | empty, and required: it is the permission gate for the above |
| `PlayerService.kt` | ExoPlayer + `MediaSession`, the library cache, and the owner of the link |
| `Library.kt` | MediaStore → tracks and playlists |
| `Browse.kt` | grouping a flat list into artist → album → track |
| `Source.kt` | what the queue was built from, encoded so it survives a restart |
| `LeadingSilence.kt` | where a track actually starts. Pure, and tested |
| `LeadingSilenceProcessor.kt` | the media3 audio processor that acts on it |
| `Keys.kt` | the XP3900 keypad, including the two softkeys the SDK has no names for |
| `Prefs.kt` | the little that outlives the process |
| `ui/` | browse, now playing, queue |

## Keys

The phone has no touchscreen, so the three softkeys under the screen are the
whole navigation scheme. They mean the same thing everywhere, because on a
keypad the only thing worse than too few controls is controls that move.

| Key | Everywhere |
|---|---|
| left softkey | settings |
| middle softkey | repeat the current queue, or shuffle it |
| right softkey | swap between the library and what is playing |

| Screen | The rest |
|---|---|
| Browse | up/down through the list, OK opens or plays, Back goes up a level. At the top of a section, **left/right switch between ARTISTS and PLAYLISTS** |
| Now playing | OK play/pause · left/right tap to skip, **hold to scrub** (8× for two seconds, then 32×) · down opens the queue |
| Queue | OK plays that entry · `*` move up · `#` move down · `0` remove |

Two of those softkeys are Sonim additions with no constant in `KeyEvent` —
`MULTIFUNC_CENTER` is 294 and `MULTIFUNC_RIGHT` is 295 on this handset, read
off the device rather than looked up. `Keys.kt` also matches on scan code, so
another keypad that reports the same physical key still works.

The transport commands on Now playing are deliberately the same three the
reserved control characteristic would carry from the handlebars.

## Known rough edges

- **Audio boost** is a system `AudioEffect`, and any handset may refuse it. A
  failure disables the effect and leaves the music playing.
- **Trim silent starts** resets when the audio sink flushes, which happens on a
  seek and a format change. Two same-format tracks played gaplessly may not
  flush in between, in which case the second lead-in survives.
- **`MediaStore.Audio.Playlists`** is deprecated from API 31. It works here
  only because the app targets 30, and it is the first thing that breaks if
  that is ever raised.
- **AGP 8.5 cannot run a Java compile step on JDK 21** — it fails transforming
  `core-for-system-modules.jar`. There is no Java source in the project today;
  the first one to appear will hit this. It is why the About dialog reads the
  version from the package manager instead of `BuildConfig`.
