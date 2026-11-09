# PC817B (U6) — usage

Single-channel phototransistor optocoupler, SMD-4. Isolates the bike's
**neutral switch** from the P4. (The datasheet here is Sharp's; the fitted
part is an HXY second source.)

## Circuit
VIN → R6 2.2 kΩ → D1 (1N4007) → opto LED → `NEUTRAL_12V` (the bike's neutral
wire; the switch grounds it in neutral). Collector → GPIO12 with R64's
pull-up; emitter to GND.

- In neutral the LED conducts, the transistor pulls the pin down: **reads
  LOW in neutral** (`bsp::kNeutralActiveLow`).
- Works on the bike (first board).

## Watch out
With the switch open, this circuit pulls the neutral wire up to ~12 V. If
anything else on the bike shares that wire with a 5 V pull-up it now sees
12 V — measure the stock harness. R6 could go to 4.7 kΩ and still saturate
the opto.
