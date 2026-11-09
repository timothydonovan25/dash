# OSRAM LR T67F.01 (LED0–LED7) — usage

Red PLCC-2 (3528) LEDs, the eight shift lamps, on the board's bottom side.
LCSC C37981853. Driven directly by PCA9955B OUT0–7 (no series resistors),
anodes on 3.3 V VCC.

## Current
- 50 mA max up to an 85 °C solder point, derating to 30 mA at 100 °C
  (datasheet v1.2 p.10). The PCA9955B's REXT caps every output at **30 mA**,
  inside the rating across the whole range — no firmware bug can exceed it.
- On USB, `ctrl` caps duty at 96/255 to stay under a 500 mA port.

## Headroom
The PCA9955B needs ~0.8 V across its output (specified at 57 mA; less at 30).
With VCC at 3.25 V, a high-Vf lamp regulates a little under 30 mA and is
slightly dimmer, not dark (pin-map §2.2 works the bins; that table was done
for the earlier LR P47F choice).

## Pattern
`shift::pattern()` in `components/core/` decides which lamps light; `ctrl`
writes the mask in one auto-increment transfer. Untested on the board.
