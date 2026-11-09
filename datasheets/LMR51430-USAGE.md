# LMR51430 (U3) — usage

TI LMR51430XFDDCR synchronous buck, 4.5–36 V in, 3 A, TSOT-23-6. Makes
**VCC = 3.3 V** for the whole board (module, IMU, GPS, PCA9955B, shift-lamp
anodes). Firmware doesn't touch it.

## Circuit
- Feedback R4 100 kΩ / R5 22.1 kΩ: VCC = 0.6 V × (1 + 100/22.1) = **3.31 V**.
  The reference is **0.6 V** (an 0.8 V figure gives 4.42 V and was wrong).
- L1 5.6 µH (PSPMAA0805-5R6M), output caps per the BOM. Stake L1 against
  vibration.
- Input: VIN through SW2, D6, F1 (1 A fuse fitted by hand), D4 SMBJ18A TVS,
  C23 220 µF/35 V hold-up (transients reach ~29 V, so ≥35 V parts).

## Budget
- VCC load idle/typical/worst: 165 / 380 / 1050 mA; eight lamps at 30 mA
  add 0.24 A. Well inside 3 A.
- Hold-up: ~236 µF on VIN gives ~9 ms from 14 V to the ~4.5 V cutoff at full
  load (~17 ms with lamps and backlight shed) — enough to stop SD writes and
  save the odometer at key-off, not for an `fsync`.

## Traps
- **USB-only power is marginal**: VIN ≈ 4.3–4.6 V after D5, at the 4.5 V
  minimum. The SD card did not mount on USB alone. Test the SD on 12 V.
- Bring-up check: VCC should read 3.31 V; if not, look at U3, R4/R5, L1.
