# MCP23017 (handlebar pod) — usage

Microchip 16-bit I2C GPIO expander, in the handlebar button pod — **off the
board**, at the end of ~0.5 m of cable. Datasheet DS20001952. Driver:
`components/drivers/src/pod.cpp`. (The board's old MCP23S17, U5 on the
first plan, was deleted; neutral and card-detect moved to P4 GPIOs.)

## Wiring
- I2C0: SDA GPIO4, SCL GPIO5, **100 kHz, not 400**: 4.7 kΩ pull-ups over the
  cable give ~470 ns rise time; fast mode allows 300 ns.
- Address **0x20** (A2..A0 low). Own bus — never shared with the PCA9955B.
- **No interrupt line**: polled at 200 Hz (`kPodPollHz`); a press is seen
  5–10 ms after it lands (two matching reads).
- Pod supply behind polyfuse F2.

## Registers used (BANK=0)
| Reg | Value | Why |
|---|---|---|
| IODIRA 0x00 | 0xFF | all inputs |
| GPPUA 0x0C | 0xFF | internal pull-ups, buttons to GND |
| IPOLA 0x02 | 0xFF | inverted, so pressed reads 1 |
| GPIOA 0x12 | read | the buttons |

## Recovery
- 10 ms I2C timeout so a pod off its connector can't stall the poll loop.
- `recover_bus` clocks a stuck SDA free at the bus's own speed (5 µs
  half-periods).
- `refresh_config()` rewrites the registers: a brown-out (e.g. after a
  jump) brings the chip back with defaults, and all buttons read pressed.
  DIAG → Bus shows I2C recover / still-stuck counters.

## Status
Works on the demo board. This board's GH1.25 connector is not fitted yet;
`tools/pod_keys.py` presses buttons over USB meanwhile.
