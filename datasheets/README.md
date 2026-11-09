# Datasheets

One PDF per device on (or plugged into) the P4 board, and a `-USAGE.md` beside
it saying how *this* project uses the part: pins, addresses, registers we
touch, values chosen and the traps found at bring-up. The USAGE notes point at
the code and docs that own each fact; if they disagree, `pins.hpp`, the driver
and `docs/p4-board-pin-map.md` win.

| Device | Ref | Role | Datasheet | Usage |
|---|---|---|---|---|
| Guition JC-ESP32P4-M3-C6 | U16 | Main module: ESP32-P4 + ESP32-C6 | `ESP32-P4.pdf`, `ESP32-P4-TRM.pdf`, `ESP32-C6-MINI-1U.pdf` | `JC-ESP32P4-M3-C6-USAGE.md` |
| NXP PCA9955B | U2 | Shift lamps + backlight current sinks | `PCA9955B.pdf` | `PCA9955B-USAGE.md` |
| TDK ICM-42688-P | U1 | 6-axis IMU | `ICM-42688-P.pdf` | `ICM-42688-P-USAGE.md` |
| CDtop PA1616D | U4 | GPS (MTK3333) | `PA1616D.pdf` | `PA1616D-USAGE.md` |
| Microchip MCP23017 | (pod) | Handlebar button pod expander | `MCP23017.pdf` | `MCP23017-USAGE.md` |
| TI LMR51430 | U3 | 3.3 V buck (VCC) | `LMR51430.pdf` | `LMR51430-USAGE.md` |
| Infineon TLE42754D | U5 | 5 V LDO for the backlight anode (LEDA) | `TLE42754.pdf` | `TLE42754-USAGE.md` |
| Sharp PC817 | U6 | Neutral-switch opto | `PC817.pdf` | `PC817-USAGE.md` |
| HT035SHV007NS panel / ILI9488 | FPC1 | 3.5" IPS TFT, 8080 8-bit | `ILI9488.pdf` | `ILI9488-USAGE.md` |
| VIEWE UEED035HV / ST7365 | FPC1 | Alternative transflective panel | `UEED035HV.pdf` | `UEED035HV-USAGE.md` |
| OSRAM LR T67F.01 | LED0–7 | Shift lamps | `LR-T67F.pdf` | `LR-T67F-USAGE.md` |

Not here on purpose: passives, connectors, and parts firmware never touches
(USBLC6-2SC6 ESD array, SMBJ/SS34/BAT54S/1N4007 diodes, fuses, the switch).
The K-line adapter's parts belong to `kline-adapter/`, a separate unordered
board. Guition publishes no datasheet for the module itself; its pinout is in
`docs/p4-board-pin-map.md` (from the github.com/megavatt05/JC-ESP32P4-M3-DEV
schematic).
