# ICM-42688-P (U1) — usage

TDK InvenSense 6-axis IMU, LGA-14. Datasheet DS-000347 v1.6. Driver:
`components/drivers/src/icm42688p.cpp`. Future vibration plan:
`docs/imu-vibration.md`.

## Wiring
- **SPI2**, shared with the SD card: MOSI 8, SCLK 9, MISO 10, CS GPIO7,
  24 MHz. INT1 → GPIO13 (data ready).
- **Every transaction is polling.** A queued transmit beside the IDF SD host
  deadlocked both and the watchdog rebooted the board. Never
  `spi_device_acquire_bus()`; the SD host holds the bus for whole commands
  and the IMU's FIFO absorbs the wait.

## Configuration (init order)
| Reg | Value | Meaning |
|---|---|---|
| DEVICE_CONFIG 0x11 | 0x01 | soft reset |
| INTF_CONFIG0 0x4C | 0x33 | big-endian FIFO/data, **I2C interface off** |
| GYRO_CONFIG0 0x4F | 0x07 | ±2000 dps, 200 Hz |
| ACCEL_CONFIG0 0x50 | 0x07 | ±16 g, 200 Hz |
| FIFO_CONFIG 0x16 | 0x40 | stream-to-FIFO |
| FIFO_CONFIG1 0x5F | 0x07 | accel + gyro + temp, 16-byte packets |
| INT_CONFIG1 0x64 | 0x00 | clear INT_ASYNC_RESET |
| INT_CONFIG 0x14 | 0x03 | INT1 push-pull, active high, pulsed |
| INT_SOURCE0 0x65 | 0x08 | UI_DRDY → INT1 |
| PWR_MGMT0 0x4E | 0x0F | gyro + accel low-noise |

WHO_AM_I (0x75) = 0x47. 0x00 means MISO held low / unpowered; 0xFF means MISO
floating (or the chip stuck in I2C mode).

## Traps found at bring-up
- The `imu` task blocks on INT1 with a timeout fallback (counted in DIAG →
  System).
- Its oscillator runs ~6 % slow: ~189 Hz by the P4's clock, not 200.
  Batches are anchored at "now" and back-dated 5 ms/sample.
- Once answered 0xFF through every reset after an SD card hot-swap until a
  power cycle (a USB/software reset doesn't power it down). Likely the I2C
  side latching onto SD traffic on shared SCK/MOSI → hence INTF_CONFIG0
  turns I2C off first thing (1.60). Not re-tested with a hot-plug.
- Axes remapped for the upright board (`tools/imu_view.py` to check). Mount
  roll/pitch calibration is still unset.
