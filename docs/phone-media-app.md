# DashMedia — the phone half of the media path

The dash cannot ask a phone what is playing. §11.1 of
`system-architecture.md` works through why: the ESP32-S3 has BLE only, which
rules out AVRCP and SPP, and the phone is a Sonim XP3900 running Android, which
rules out Apple's AMS. What is left is an app on the phone pushing strings
over one GATT characteristic.

This is that app. It is a local-file music player, and it is the bridge that
feeds `core::state::Media`. Source in `phone/dashmedia/`.

§11.1 is the contract and this document does not amend it. Where something
here is an interpretation rather than a quote, it says so.

## The phone is the constraint

Sonim XP3 Plus (XP3900): Android 11 Go Edition, 2.8" 240×320, no touchscreen,
D-pad and keypad, 2 GB RAM. Every decision below comes from that list.

| Decision | Because |
|---|---|
| Views and `RecyclerView`, not Compose | 2 GB, Go edition, and the whole UI is a list |
| `targetSdk 30`, matching the device | Sideloaded, never on Play. Keeps the legacy permission model and pre-34 foreground-service rules — none of which this phone has — so there is nothing to reason about that the device will not actually do |
| Flat track list, sorted | Artist → album → track is two more D-pad screens between you and a song |
| Dark, 16 sp rows, ~7 to a screen | 240×320 read at arm's length, outdoors |
| One foreground service for playback *and* BLE | Go kills background processes; a `MediaSessionService` is foreground whenever music plays, so the link is alive exactly when it has something to say |
| No bonding | The characteristic is write-no-response with no auth. Pairing would add a failure mode and protect song titles |

Audio still goes phone → helmet over A2DP. The dash has no audio hardware; it
is a display.

## What it does

```
  MediaStore ──► Library ──► ExoPlayer ──► MediaSession ─┐
                                                         ├─► Feed ─► DashLink ─► BLE ─► dash
  Spotify, podcasts, anything ──► MediaSessionManager ───┘
```

The bridge reads the active `MediaSession`, not its own player, so anything
else on the phone reaches the handlebars too. That needs notification-listener
access — `MediaSessionManager.getActiveSessions()` is gated on it, which is
the only reason `NotifListener` exists. Without the grant the app falls back
to a `Player.Listener` on its own ExoPlayer: your own music still reaches the
dash, Spotify does not. The app is worth having either way, so the grant is
never a blocker.

```
adb shell cmd notification allow_listener com.kao.dashmedia/.NotifListener
```

is the same grant, without navigating Settings by D-pad.

## The protocol, as implemented

`Feed.kt` is the only file that knows the wire format, and it has no Android
imports — the same discipline `components/core/` follows, for the same reason:
the part that has to be right is the part that must be cheap to test.

- **Tag, one space, value.** §11.1's examples all read `T The Devil In I`.
  *Interpretation:* the firmware parser must skip exactly one space, not
  `lstrip`, or a title with leading whitespace changes on the way through.
  This is the one place the two ends could quietly disagree.
- **Truncate to 59 bytes on a character boundary.** The dash truncates too,
  but at a byte count — it cannot know where a character started. Cutting here
  as well is the difference between a title that stops early and one that ends
  in a replacement glyph.
- **An empty value is legal and means "clear".** A track with no album sends
  `"L "`, because otherwise the previous track's album stays on screen.
- **Send only what changed.** A `MediaController` callback fires on every
  position update and changes none of the four fields.
- **Resend all four on connect.** §11.1 requires the dash to zero `Media` on
  disconnect, so after a drop it genuinely knows nothing.
- **Re-send `S` every 10 s.** A liveness beat, and the cheapest repair for a
  dropped write. Reset by any real state change, so it never doubles up.

## The three things Android will punish you for

In `DashLink.kt`, and none of them are style:

1. **MTU.** Android's default ATT MTU is 23 — 20 payload bytes. The dash's
   `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` is 256, but only the central can start
   the exchange. Nothing is written until `requestMtu(256)` has come back, or
   §11.1's "no chunking" rule is false and every long title is cut at 20 bytes.
2. **One operation in flight.** `BluetoothGatt` permits exactly one outstanding
   request, write-without-response included. A second `writeCharacteristic`
   before `onCharacteristicWrite` returns `false` and the line is simply gone.
   A track change is four lines, so this is the normal case, not the edge one.
   Hence the queue, which drops its oldest entry when full: if the link is
   stalled, what is playing now beats what was playing then.
3. **`ACCESS_FINE_LOCATION`.** On API 30 a BLE scan without it returns an empty
   result and no error at all. It has nothing to do with location here.

**For the dash side, when it is written:** the peripheral must put the 128-bit
service UUID in its advertisement, because that is what the scan filters on.
One 128-bit UUID is 18 of the 31 available bytes, which leaves room for flags
and a short name and not much else.

## Reserved: control from the handlebars

Designed now, built later, so the number is settled before anything depends
on it.

```
Service  6e5f0001-b5a3-f393-e0a9-e50e24dcca9e   "Dash Media"
  char   ...0002  Feed   write-no-response   phone -> dash   (§11.1, built)
  char   ...0003  Ctrl   notify              dash  -> phone  (reserved)
```

Same tagged-line style, opposite direction:

| Line | Meaning |
|---|---|
| `C 0` | previous track |
| `C 1` | play / pause toggle |
| `C 2` | next track |

`DashLink` would subscribe and turn a line into a
`MediaController.TransportControls` call — which works for Spotify as well as
for our own player, because the session is the abstraction. Unknown `C` values
are ignored, on the same reasoning as unknown tags.

Nothing on the dash changes for this yet. When it does: media currently lives
on the CRUISE home screen (`ui/src/screens_cruise.cpp`), and `core::nav`
already has C/D focus and E accept free, so three focus rows are the natural
control surface and no button has to be taken from anything else.

## Bring-up order

The point of this order is that only one unproven end is ever in play.

1. **`Feed` on the JVM.** `./gradlew :app:testDebugUnitTest` — no device, no
   emulator, and it is where the protocol is actually pinned: truncation on a
   multibyte boundary, dedupe, resend-on-connect, the beat.
2. **Against `tools/dash_media_peripheral.py`.** A laptop BLE peripheral
   advertising the §11.1 service, enforcing the same 59-byte truncation and
   zeroing its mirror on disconnect. It prints what the CRUISE screen would
   show. `pip install bless`; needs BlueZ and an adapter that can advertise.
   This proves scan, connect, MTU, queue and encoding before the PCB has a
   radio at all.
3. **On the phone.** `adb install`, grant notification access, play a local
   file and watch the peripheral print `T`/`A`/`L`/`S`. Then start Spotify and
   watch it print those too. Then walk out of range and back, and confirm all
   four fields arrive again.
4. **On the dash**, once `services/ble/` exists. The DIAG screen already has a
   Media row (`ui/src/screens_diag.cpp`), so first light is one line of text on
   a screen that already works.

## Known gaps

- The XP3900 specification here is from vendor and retail listings. Confirm the
  Android build and whether the unit has a headset media-button path.
- `services/ble/` on the dash is step 9 of the build order and is not written.
  Step 2 above is what keeps this honest in the meantime.
- Dependency versions in `gradle/libs.versions.toml` have never been resolved —
  there is no Android SDK on the machine this was written on. Expect to bump
  them the first time Android Studio opens the project.
