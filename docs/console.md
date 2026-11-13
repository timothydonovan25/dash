# USB console and hands-off testing

The P4's USB-C is its USB-Serial/JTAG: flashing, the log, and a line-based
console (`main/serial_buttons.cpp` on the bike, `demo_p4/main/serial_buttons.cpp`
on the demo). This is how the first board was tested with nobody at it
(2026-10-31/11-01), and how to work on it without the handlebar pod.

## Opening the port without resetting the board

pyserial lowers DTR before RTS, and DTR low with RTS high is the
USB-Serial-JTAG's reset. Open with both `True`, then set `rts = False`
before `dtr = False` (`tools/pod_keys.py` does). The reverse order (DTR
low, then RTS low) resets it on purpose. On the demo board an esptool
reset can wedge it in a boot loop; only unplugging fixes that.

## Commands

Send a bare newline first: half a line left by an earlier session is
dropped after 500 ms, but only if nothing follows it sooner.

| Command | Board | Does |
|---|---|---|
| `btn A` … `btn E` | both | A short press, posted after debounce |
| `long A` … `long E` | both | A long press, after debounce |
| `hold <0-31>` | bike | The buttons held now, bit 0 = A. Repeated while any is down; lapses after 1 s without one. Debounced and decoded like the pod, and ORed into `Deps::buttons_held`, so Doom plays from it |
| `odo <miles>` / `hours <h>` | bike | Sets the odometer or the engine hours (after an NVS erase or a board swap); saved the next time the bike is still. Not on the dash anywhere |
| `tpms sniff [dBm]` | bike | Logs for 60 s every BLE advert at -70 dBm (or the floor given) or nearer, as hex, and what `core::tpms` decodes from it (features §24.4). Hold the sensor by the dash |
| `pod trace` | bike | Logs the raw pod pins and decoded presses for 30 s |
| `crash test` | bike | Starts the crash countdown as a crash would; at 0 the SOS goes to the phone (features §24.3). Any button cancels |
| `c6 hold` / `c6 release` | bike | Holds the C6 in reset: what a dead radio does (`ble: radio lost`, no reboot) |
| `panic` | bike | `abort()`: the core dump, the reboot, a dev image falling back to stable |
| `inputs` | demo | Traces raw pod pins, touch zones and decoded presses for 30 s |
| `state` / `regs` | demo | Dumps the state slots / the crypto reset registers |

## `tools/pod_keys.py`: the pod from the laptop

```bash
. ~/.espressif/v6.0.2/esp-idf/export.sh     # for pyserial
python3 tools/pod_keys.py [-p /dev/ttyACMn]
```

On a terminal that reports key releases (kitty, or another with the kitty
keyboard protocol), keys 1–5 *are* A–E, held while held, sent as `hold`
lines, so debounce, long presses and Doom all work. Anywhere else it sends
decoded presses: 1–5 short, shift+1–5 long; debounce and hold timing are not
exercised. q quits. The board's log scrolls alongside.

To flash without quitting it, pause it: `SIGUSR1` sends `hold 0` and lets go
of the port, `SIGUSR2` reopens it (re-finding the board's port unless `-p`
was given, waiting up to 30 s for it to enumerate) and keys work again:
`kill -USR1 $(cat $XDG_RUNTIME_DIR/pod_keys.pid)`, flash, then the same with
`-USR2`. Not `pkill -f pod_keys.py`: it also signals any shell whose command
line names it, and SIGUSR1 kills a shell.

## Menu routes

From anywhere: `long A` is CRUISE screen 0. Presses 350 ms apart. If one
stops working, the sim shows the same screens.

| What | Presses |
|---|---|
| Wi-Fi on/off | `long A`, A×4, D×6, E, D×2, E — then E again toggles |
| Eject card | `long A`, A×4, D×6, E, E |
| DIAG → Update | `long A`, A×3, D×7, E |
| Boot dev once | then D, E, E |

These counts follow CONFIG's sections (`kSections` in `core/src/config.cpp`;
Card & Wi-Fi is the seventh) and DIAG's menu; a new section, a setting
added ahead in that section, or a new page moves them.

## Before fuzzing the menus, back up NVS

NVS holds settings, the phone's bond, trip and waypoints:
`esptool read-flash 0x11000 0x10000 nvs.bin` twice and compare; afterwards
`esptool write-flash 0x11000 nvs.bin` and read it back. Random presses reach
Forget phone, Calibrate level, the trip and gear resets. Tracks need a fix,
so indoors they are safe.

**They reach the slots too.** Before 2.14, Boot dev once and Promote to
stable each acted on one short E; a 20 min run did both (dev booted at
6 min, promoted at 14), leaving the older dev image as stable. They now want
E twice with nothing between, which random presses hit far less often, but
still check the boot line `ota: running ... from ota_N` afterwards and, if
it moved, `idf.py flash` (it rewrites otadata, so stable is `ota_0` again).

## The 5 s status block

Every 5 s, while a USB host is connected, `keeper` prints a header and one
line per service: `ctrl`, `imu`, `gnss`, `logic`, `input`, `render`,
`logger`, `ecu`, `storage`, `keeper`, `power`, `ota`, `ble`, `wifi`. It replaced
the 1 Hz `STAT` line and the 10 s `heap`/`imu`/`timing`/`gnss`/`power`/
`logger` summary. What a soak watches:

- `keeper` — `heap` free is internal RAM; a leak is it falling. Also the
  task whose stack came closest to full.
- `imu` — most samples found waiting at one wake, and how many wakes found
  over 64 and over 100 (the FIFO holds ~128).
- `ctrl` late ticks, `render` rate.
- `logger` — rows, bytes, slowest write, and rows each ring dropped since
  boot (blackbox/imu/tach/det/evt).

Each line starts with its device's health (`ok`, `FAULT`, ...). Events
(warnings, console replies, boot lines) still print when they happen. At
boot `sd` gives the card's size and free space, and `logger` the session
picked and how long the scan took; every task's stack high-water mark is
listed once at 10 s of uptime (`services::watch_stacks`).

## What it found on the first board

- 19 min and 2337 random presses, no reset.
- Wi-Fi: 20 on/off cycles, RAM back each time.
- Eject now flushes the ~20 kB the logger used to drop.
- A lost C6 no longer reboots the dash.
- Dev boot, reset and panic all fall back to stable.
- 40 min soak on 1.61: heap flat at 306 kB free, 0 late `ctrl` ticks,
  render at 120 Hz, 76 MB to the card with 0 errors, the IMU at most 77
  samples behind.
- 2 h clean (heap flat, `ctrl` late 0), but the IMU reached 116 behind after
  ~1.4 h: a card write holding SPI2 near the FIFO's ~680 ms.

**The raw IMU log's block size, A/B,** one hour each on the same card:

| Block | Peak behind | Wakes over 64 / 100 | Slowest write |
|---|---|---|---|
| 16 kB | 144 | 6 / 4 | 925 ms |
| 8 kB | 83 | 4 / 0 | 742 ms |
| 8 kB, second hour | 130 | 3 / 1 | 788 ms, no ring dropped a row |

8 kB it is: it makes the long holds rarer, not impossible. The FIFO is
stream mode (`FIFO_CONFIG` 0x40): a full one drops whole old packets, and
samples are dated from the newest, so an overflow is a gap in `_imu`, not
scrambled data. One run each; the card's own garbage collection still sets
the worst case.

## Screen and SD card over USB (2.6)

- `screen`: the dash's copy of the glass as text lines (`SCREEN BEGIN`,
  one `SCR <y> <base64 runs>` a row, `SCREEN END`). `tools/dash_screen.py
  [-o screen.png] [--live 1]` saves it; `tools/pod_keys.py --screen 1
  [--shot screen.png]` does it alongside the buttons, on the one port. It
  shows what the dash meant to draw, not the glass: a clean shot of a
  garbled screen points at the panel bus.
- `usb drive`: the SD card as a USB drive on the laptop, parked only
  (CONFIG → SD card → USB drive does the same). The console goes away
  while it is a drive. When the laptop ejects it (or it is unplugged, or
  nobody claims it in a minute), the dash restarts, ~2 s, parked: the USB
  serial driver did not survive its pads going off and on, and a boot
  brings the console and the card's mount back clean. A power cycle always
  brings the console back too (drivers::UsbDrive).
  **Eject, not power off.** `eject /dev/sdX` or a file manager's Eject
  ends the session. `udisksctl power-off` ("Safely remove" on Linux) sends
  SCSI STOP without the eject bit, which esp_tinyusb ignores, then drops
  the device: the dash stays a drive nobody is using, with no console
  (2026-11-06). Out of it: CONFIG → SD card → USB drive again, or a power
  cycle. Not fixable by wrapping `tud_msc_start_stop_cb`: TinyUSB's
  `msc_device.c` calls it in the same file as its weak default, so
  `--wrap` never sees the call.
- `route bench`: a Fayetteville → Little Rock route through the card, cold
  then warm, timed in the log (navigation §9).
