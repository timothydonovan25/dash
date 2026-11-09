# PA1616D (U4) — usage

CDtop GNSS patch-antenna module, MediaTek MT3333, LCC-20. **Hand-fitted**
(not in the BOM). Datasheet v.05. Commands are MTK PMTK sentences (the PMTK
command manual, not this datasheet). Driver:
`components/drivers/src/pa1616d.cpp`.

## Wiring
- UART1: P4 RX GPIO2 ← GPS TX, P4 TX GPIO3 → GPS RX (U4-10).
- RF: U4-11 → JP1 (U.FL) for an external antenna; trace widened to ~47 Ω.
- **PPS not wired** on purpose: time comes from RMC, lap crossings are
  interpolated between fixes.
- **VBACKUP (U4-4) tied to VCC**, so every power-up is a cold start. A
  supercap/coin cell through a diode would give hot starts.

## Startup sequence
1. Boots at 9600 baud, 1 Hz (because no backup supply).
2. `PMTK251,57600` (not acknowledged; the switch is proven by listening at
   57600). Sent twice before giving up and staying at 9600.
3. `PMTK314,0,1,0,1,0,…` — RMC + GGA only.
4. `PMTK313,0` — SBAS off.
5. `PMTK220,100` — 10 Hz fixes. Each acked by `PMTK001` within ~500 ms.

10 Hz of RMC+GGA+VTG+GSA is ~2.4 kB/s — 42 % of 57600 and overruns 9600,
which is why the baud switch comes first.

## Status / open problem
Fixes with 9–13 satellites, but **stuck at 1 Hz: the receiver hears nothing
the P4 sends**, even at 9600. The firmware side is proven (GPIO3 drives UART1
TX). Check U16-6 → trace → U4-10 and its joint, then the logic level (3.3 V
P4 into a 2.8 V-I/O module). `docs/bringup.md` §7, pin-map §6.2.
A cold receiver's placeholder date is ignored by the parser.
