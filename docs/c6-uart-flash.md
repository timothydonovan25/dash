# Flashing the C6 over its own UART

The module's ESP32-C6 normally gets its firmware from the P4 over SDIO
(`slave_fw`, `core/c6`). That needs the C6 to already speak ESP-Hosted. The
first bike board's C6 came with **blank flash**, so it had to be flashed
over its UART once (2026-10-31). Expect the same on the next module. This
is that procedure; `tools/c6_uart.py` automates most of it.

Symptom of a blank C6, from the P4's console: `sdmmc_card_init` succeeds
and the next CCCR read times out (`sdio_card_fn_init failed`, `0x107`), at
40 MHz 4-bit and at 5 MHz 1-bit alike. On the C6's own UART:
`invalid header: 0xffffffff` and watchdog resets several times a second.

The C6 has its own reset (GPIO54, U16-74) and boot (U16-18, R1) lines.
Its UART is on test pads **H1 = U0RXD** (U16-16, into the C6) and
**H3 = U0TXD** (U16-17, out of it); its boot pin has no pad, so forcing it
into download mode means clipping onto R1.

**Found on the first board (2026-10-28).**
`sdmmc_card_init` succeeds and the very next CCCR read times out
(`sdio_card_fn_init failed`, `0x107`), the same at 40 MHz 4-bit and 5 MHz
1-bit, with the demo's identical ESP-Hosted settings. So it is the
module's C6, not the link, and the SDIO push from `slave_fw` cannot reach
it. **Its flash was blank** (2026-10-31: the 4 MB read back held 24 bytes
that were not 0xFF, no bootloader, no partition table): on its UART it
printed `invalid header: 0xffffffff` and watchdog-reset (`TG0_WDT_HPSYS`,
`LP_WDT_SYS`) several times a second. Expect the same on the next module.
Flash the C6 over its own UART once:

With an FTDI TTL-232R-3V3: **black** GND, **yellow** its RXD, **orange**
its TXD. **Red is 5 V: leave it unconnected.** It ends up as
`/dev/ttyUSB0`. Source ESP-IDF (`export.sh`) and run from the repo root.
`tools/c6_uart.py` does steps 1, 4, 6 and 7 (`listen`, `wait`, `backup`,
`flash`); `flash` refuses to run until a 4 MB backup exists, and
`inspect` reads the backup offline: bootloader, partition table, each app's
project, version (ESP-Hosted's own, for a `network_adapter`) and IDF, and
whether it was built for SDIO or SPI.

1. **Listen.** Board off. Black to GND, **yellow to H3** (the C6's TX).
   Open `idf.py -C demo_p4/c6_cp -p /dev/ttyUSB0 monitor --no-reset`, then
   power the board. Do not open the P4's `/dev/ttyACM0` meanwhile: that
   resets the P4, which resets the C6. The text is the first evidence of
   what Guition put on it; keep it.
2. **Wire for flashing.** **Orange to H1** (the C6's RX).
3. **Find R1's IO9 end.** Board off, meter on continuity: the end of R1
   that beeps to a 3.3 V point (VCC) is the wrong one; the other is IO9.
   Ground the IO9 end **through a 1 kΩ resistor**, not a bare wire: it
   still reads low (0.3 V against the 10 kΩ pull-up), and the wrong end
   would only pass 3.3 mA instead of shorting VCC.
4. **Download mode.** The C6's net names: **C6_BOOT is IO9** (on R1),
   **C6_RST is EN** (on R22, 10 kΩ pull-up and 1 µF). IO8 has no net; the
   module holds it high. With the 1 kΩ on C6_BOOT (it reads ~0.3 V),
   **tap C6_RST's signal end of R22 to GND** for half a second. The C6's
   own watchdog resets do not re-read the straps, so a blank C6 cycling
   through them stays at `boot:0xc (SPI_FAST_FLASH_BOOT)` however long you
   wait; only EN (or power-on) brings `rst:0x1 (POWERON),boot:0x4
   (DOWNLOAD(USB/UART0/SDIO_FEI_FEO))` and `waiting for download`. Only tap:
   the P4 drives GPIO54 too. Check with
   `python -m esptool --chip esp32c6 -p /dev/ttyUSB0 --before no-reset --after no-reset chip-id`.
   `boot:` on the C6's UART is the strap reading: 0xc is both high,
   0x4 is IO9 low.
5. **Park the P4.** On the first board the P4 kept resetting the C6
   through GPIO54, a pair of power-on resets every ~14 s, and each one
   killed a long esptool read part way (`No more data to read from the
   serial port`). Put the P4 in its own ROM bootloader, where GPIO54 is
   left alone:
   `python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before default-reset --after no-reset chip-id`.
   Listen to the C6's UART for 20 s: no new `ESP-ROM` banner means
   nothing is resetting it.
6. **Back up what is there**, 4 MB. At 115200 it took 6.5 minutes;
   460800 failed once, before the P4 was parked, so which of the two was
   to blame is not known:
   `python -m esptool --chip esp32c6 -p /dev/ttyUSB0 -b 115200 --before no-reset --after no-reset read-flash 0 0x400000 c6_factory_backup.bin`
   (`tools/c6_uart.py backup` does the same at 460800).
   `tools/c6_uart.py inspect` says whether it holds anything.
7. **Flash ESP-Hosted** (`demo_p4/c6_cp`, 2.12.8, Wi-Fi and BLE; build it
   first with `idf.py -C demo_p4/c6_cp build`), about 70 s at 115200:
   `cd demo_p4/c6_cp/build && python -m esptool --chip esp32c6 -p /dev/ttyUSB0 -b 115200 --before no-reset --after no-reset write-flash "@flash_args"`
8. **Run it.** Unclip the 1 kΩ from R1, unplug and replug the board (the
   P4 is parked), and read the P4's console. Expect `transport: Identified
   slave [esp32c6]` and the C6 reporting 2.12.8, then `ble: advertising`.
   The yellow wire can stay on to watch the C6's side at the same time.
   First board, 2026-10-31: all of that, SDIO at 40 MHz 4-bit, and the
   P4 booted once with no loop.
9. **Then `idf.py -p /dev/ttyACM0 c6-flash`**, so `slave_fw` holds the same
   image and later C6 updates go over SDIO, as on the demo. The boot log
   then says `c6_fw: C6 runs 2.12.8 #…, slave_fw has 2.12.8 #…: current`.

