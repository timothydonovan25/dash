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
| `PlayerService.kt` | ExoPlayer + `MediaSession`, and the owner of the link |
| `Library.kt` | MediaStore → tracks |
| `ui/` | three D-pad screens: library, now playing, settings |

## Keys

The phone has no touchscreen.

- **Library** — D-pad up/down, centre plays. `MENU` jumps to now playing, `0` to settings.
- **Now playing** — left previous, right next, centre play/pause.

Those three transport commands are deliberately the same three the reserved
control characteristic would carry from the handlebars.
