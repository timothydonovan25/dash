# K-line to CAN adapter — netlist review

> **History: the first K-line adapter (CAN output), superseded by the
> ESP-NOW adapter in `kline-adapter/review.md`.** §1 still holds for the P4
> dash: it has no CAN transceiver, and the P4's TWAI is logic-level only.
> Kept for the reasoning.

Source: `Sheet_1_202608121.net`, 24 parts. This matches the Aug 12 2026
rev 2 board (35.43 × 20.32 mm).

**This is the adapter board, not the dashboard.** The dashboard netlist is
a different file — around 165 parts, ESP32-S3, ILI9488 FPC, MCP23S17.

Items already closed in earlier sessions are not raised again here: the
USB / main-board supply OR-ing, the mounting holes, and the U5 silkscreen
label.

---

## 1. Blocking — the dashboard cannot receive CAN

The dashboard PCB now at fab has **no CAN transceiver**. Its part list is
ESP32-S3-WROOM-1, two LMR51430, MCP23S17, PA1616D, ICM-42688-P, AMS1117,
TPS2117, USB-C, USBLC6-2SC6, SMBJ18A, the SD socket, the 40-pin FPC, a
PC817B, the shift-lamp FETs, and the connectors. There is no SN65HVD230,
no TJA1050, no MCP2551.

The ESP32-S3 has a TWAI controller, but TWAI outputs logic-level TX/RX.
It cannot drive a differential CAN pair. Wiring `CANH`/`CANL` from this
adapter to two ESP32-S3 GPIOs will not work, and putting 5 V-swing CAN
levels onto 3.3 V pins may damage them.

So the adapter, as drawn, can only talk to a dashboard revision that does
not exist yet. Four ways forward:

| Option | Change needed | Verdict |
|---|---|---|
| **UART over the same 4-wire cable** | Adapter firmware only. Reuse `CANH`/`CANL` pins as TX/RX. Dash uses two of its three spare pins (U2-6, U2-31, U2-32). | Works today. Zero hardware change on either board. |
| **BLE link** | Firmware only, both ends. The dash already runs NimBLE. | Works today, no wires, but adds latency and radio contention, and you said the adapter should not use its radio. |
| **Transceiver on the dash** | A dead-bug SN65HVD230 on the spare pins, or wait for dash v2. | Correct long-term, not available now. |
| **Do nothing until dash v2** | None. | The `NullEngine` stub already covers this. |

The `IEngine` interface in the architecture exists exactly for this. Any
of these is one implementation file; nothing else in the dash changes.

**Note on the traces if you go UART:** the adapter's `CANH` and `CANL`
nets run through R5 and U2 before reaching U5. You cannot repurpose them
without cutting the board. A UART link would need two different pins
brought to U5, which means a respin of the adapter. Weigh that against
just adding a transceiver to dash v2.

---

## 2. Electrical risk — L9637D running at 3.3 V

`U3-3` (VCC) is on the 3V3 net from the TLV76133. The L9637D is a 5 V
part. Check the datasheet's operating VCC range; if it reads 4.5–5.5 V,
this design is off-spec.

It may well work on the bench at room temperature. Off-spec supply rails
usually fail later, when hot, and intermittently, which is the worst kind
of bug to chase on a bike.

Two fixes, both available because 5 V is already on the board:

1. **Move `U3-3` to `5VIN`.** Then `KL_RX` swings to 5 V, and ESP32-C6
   GPIOs are **not** 5 V tolerant, so add a divider on `KL_RX` (10k/15k
   gives 3.0 V). Also confirm the L9637D's TX input threshold accepts a
   3.3 V high when VCC is 5 V — check `VIH` on the datasheet.
2. **Leave it at 3.3 V and prove it.** Bench-test the link with the heat
   gun on the board, at both temperature extremes you expect. If it is
   solid, accept it and write the decision down.

Option 1 is two passives and one net change. Option 2 is free but you
carry the risk into the harness.

---

## 3. Verify — TLV76133DCYR pin 4

The netlist has `U4-1` on GND, `U4-3` on `5VIN`, and **both `U4-2` and
`U4-4` on VCC**. There is no EN net anywhere in the design.

That is consistent with pin 4 being the SOT-223 thermal tab tied to the
output, and the part having no external enable in this package. Confirm
it on the datasheet.

If pin 4 turns out to be **EN**, the board will not start. EN tied to the
regulator's own output means: output is 0 V at power-up, so EN is 0 V, so
the regulator stays off, forever. That is a five-minute check and a dead
board if you skip it.

---

## 4. 12 V input has no protection

`12VIN` goes from `U6-3` straight to `U3-7` with only C8 (100 nF) on it.
No fuse, no TVS, no reverse-polarity block, no bulk capacitance.

You already solved this on the dashboard: F1 fuse, SMBJ18A TVS, SS34
series Schottky. The same three parts belong here. Your own note in the
project says the dirtbike 12 V line is very noisy and spikes above 12 V.

Minimum worth adding:

- **SS34 in series** on `12VIN`. Reverse-polarity block. The 0.5 V drop
  does not matter to the L9637D's VS pin.
- **SMBJ18A** from `12VIN` to GND, after the diode. Load-dump clamp.
- **10 µF** bulk alongside C8.

A fuse matters less here than on the dash, because the only 12 V load is
the L9637D. The diode and the TVS are the two that save the part.

Also confirm with a meter whether the DLC 12 V pin is ignition-switched,
as you suspected. If it is always live, the L9637D's quiescent draw sits
on the battery for as long as the bike is parked.

---

## 5. K-line pull-up

ISO 9141-2 expects the tester side to pull K up to battery through about
510 Ω. Check whether the L9637D provides this internally on its K pin. If
it does not, one resistor from `KL_SERIAL` to `12VIN` is the whole fix,
and without it the bus may never idle high and the ECU will not answer.

Test this before you trust the board: with the adapter connected and
powered but idle, `KL_SERIAL` should sit near battery voltage.

---

## 6. Things that look right

Worth saying, so you do not go looking:

- **USB data pairs.** `USB1-2 → D1-3 → D1-4 → D-` and
  `USB1-3 → D1-1 → D1-6 → D+`. Micro-USB pin 2 is D− and pin 3 is D+, and
  ESP32-C6 uses IO12/IO13 for USB. `U1-17`/`U1-18` matches. Correct, and
  the ESD part is in the right place — on the connector side, not the
  MCU side.
- **EN circuit.** R4 10 k pull-up plus C9 1 µF plus SW1 to GND on `U1-8`.
  Standard, and the 1 µF gives a clean power-on reset.
- **Boot strapping.** R1 pulls `U1-22` high, R2 pulls `U1-23` high with
  SW2 to GND. That is the usual IO8-high / IO9-boot-button arrangement
  for a C6. Verify the two pin numbers against the module drawing, but
  the shape is right.
- **CAN Rs.** R6 10 k from `U2-8` to GND puts the SN65HVD230 in slope
  control rather than high speed. For a short link that is a sensible
  EMI choice, not a mistake. Use 0 Ω only if you need the full bitrate.
- **Decoupling.** C1/C2 on 5VIN, C3/C4 at the LDO output, C5 at the
  module, C6 at U2, C7 at U3. One per rail per chip. Good.

---

## 7. R5 — the 120 Ω terminator

`R5` hard-wires 120 Ω across `CANH`/`CANL`. A CAN bus wants exactly two
terminators, one at each physical end.

- Point-to-point adapter ↔ dash: this board has one, the dash end needs
  the other. Two total. Correct.
- If you ever tap this adapter onto the bike's own CAN bus, or add a
  third node, that fixed 120 Ω becomes a third terminator and the bus
  degrades.

Cheap insurance on the next respin: make it a solder-bridge jumper in
series with R5. One pad pair, no cost.

---

## 8. Summary

| # | Item | Severity | Fix cost |
|---|---|---|---|
| 1 | Dash has no CAN transceiver | Blocking | Firmware, or dash v2 |
| 2 | L9637D VCC at 3.3 V, likely off-spec | High | 1 net + 2 resistors |
| 3 | TLV76133 pin 4 unverified | High if wrong | Datasheet check |
| 4 | No 12 V protection | High | SS34 + SMBJ18A + 10 µF |
| 5 | K-line pull-up unverified | Medium | Datasheet check, maybe 1 R |
| 6 | R5 termination not removable | Low | Jumper on next respin |

Items 3 and 5 are datasheet reads and cost nothing. Do those first — one
of them decides whether the board powers up at all.
