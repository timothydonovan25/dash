One component per chip. A driver knows its chip and nothing about the app,
reports health, and decides no policy (rule 5).

| Driver | Chip / job |
|---|---|
| `ili9488` | The panel on the 8-bit i80 bus, via the `esp_lcd_ili9488` managed component (IPS init for the HT035; VIEWE's ST7365 table for the UEED035HV, told apart by which WR/RS wiring makes TE pulse). Forwards the backlight to `pca9955b` |
| `pca9955b` | Shift lamps (OUT0–7) and backlight (OUT8–10), I2C1 at 1 MHz. `init()` rewrites every register: its reset is on the P4's EN |
| `icm42688p` | IMU on SPI2, FIFO-driven, every transaction polling (never a queued transmit beside the SD host) |
| `pa1616d` | GPS on UART1, bytes only (the parser is `core/nmea`); baud negotiation that proves itself |
| `pod` | Handlebar pod, MCP23017 on I2C0 at 100 kHz, polled; `recover_bus` clocks a stuck bus free |
| `engine_sense` | Tach and both wheel Halls (one driver: the same three edge counters behind one latched read) and neutral |
| `vin_sense` | VIN divider on ADC1: bike vs USB, key-off |
| `sdcard` (+ `sdcard_spi.cpp`) | The card and its one lock; the mount is behind `sdcard_mount.hpp` (SPI here, SDMMC on the demo) |
| `chip_temp` | The P4's die temperature |
| `nvs_storage`, `clock` | NVS, and time |

They compile clean for esp32p4 against IDF v6.0.2. **Only some has run on
the bike board** — `docs/bringup.md`'s status table is the record. The rest
is written against datasheets and the netlist; every place that can only be
settled on a bench is marked `BRINGUP` (bringup §10 lists them), e.g. the
IMU axis mapping (bench only, not ridden), the tach refractory window, and
`ButtonPod::recover_bus` (never freed a real stuck slave; DIAG → Bus counts
its recoveries and the ones that left SDA low).

Pin numbers come only from `components/bsp/include/bsp/pins.hpp`. A driver
adds a device to a bus `bsp/buses.cpp` already made.

Work down DIAG → Sensors and DIAG → Bus. Those screens exist for bring-up.
