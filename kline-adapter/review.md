# K-line adapter (smaller, ESP-NOW) — netlist and plan review

> **Status.** A separate board, not the dash: it reads the ECU's K-line and
> would send it to the dash's C6 over ESP-NOW. Reviewed through revision 3
> and an independent audit (2026-10-14, §16); the repo records no order or
> bring-up. **The dash side is written (2.15, features §35) and has not run
> against an adapter.** It differs from §10 below in two ways the adapter's
> firmware must follow: the adapter **broadcasts** (no unicast, no LMK, no
> pairing; the dash takes the first adapter it hears each power-on), and it
> sends the ECU's **raw table bytes** in the frame `core/ecu/ecu.hpp`
> defines, on channel 6. `gen_sch.py` and `pcb_drc.py`
> beside this file generate and check the schematic and PCB.

Source files, all 2026-10-13: `Sheet_1-copy_2026-10-13.net`,
`PCB_PCB_K-Line-to-CAN-smaller_2026-10-13.json`, the Gerber zip and the BOM.
14 parts, 41.7 × 22.2 mm.

This supersedes `docs/history/kline-can-adapter-netlist-review.md` for the new board. The
SN65HVD230, the 120 Ω terminator and the TLV76133 are gone, so that review's
§1 (no CAN on the dash), §3 (LDO pin 4) and §7 (R5) no longer apply. The
link to the dash is now **ESP-NOW**, adapter C6 → dash C6.

---

## 1. Blocking — 5 V on the module's 3V3 pin

The `VCC` net is the 78M05 output (`U2-3`) and it also feeds `U1-3`, the
ESP32-C6-MINI-1's 3V3 pin. The C6's absolute maximum is 3.6 V. **The
first power-up will destroy the module.**

The same net feeds `U3-3` (L9637D VCC), which is correct: the L9637D is a
5 V part, so the old review's §2 is fixed by this board. What is missing is
a separate 3.3 V rail for the module.

Fix: a 3.3 V LDO from the 5 V rail to `U1-3` (the TLV76133 from the old board
is fine, or an AP2112K-3.3), with its own 10 µF. The 5 V rail then feeds the
L9637D only.

## 2. Blocking — USB VBUS is shorted to bike 12 V, and so is D1

`VIN` joins `USB1-1` (VBUS), `D1-5`, `U6-3` (DLC 12 V), `U3-7` (L9637D VS) and
the 78M05 input. The adapter is never on the bike and the laptop at the same
time, so the laptop is safe. Two problems remain:

- **D1 on the bike.** `D1-5` is the USBLC6-2SC6's VBUS pin. It has an
  internal Zener to GND that breaks down at about 6 V. On the bike it sits
  across 12–14 V with nothing to limit the current. D1 burns, and it
  probably fails short across `VIN`.
- **USB alone browns out.** The 78M05 sees 5 V in and gives roughly 3 V
  out, because of its ≈2 V dropout. Any 3.3 V LDO after it then gives less
  than the C6's 3.0 V minimum.

Fix: take VBUS off `VIN`. `D1-5` and `USB1-1` become their own net, which
reaches the logic rail through a Schottky. The regulator output gets a second
Schottky, or the regulator stage takes a diode-OR at its input. On USB alone
the K-line is dead because VS is 0 V, which is fine for flashing.

## 3. Blocking — C1 is a 10 V capacitor on the 12 V rail

C1 is `CL10A106KP8NNNC`. The `P` is a 10 V rating, and C1 sits across `VIN`.
That is 14.4 V while the bike charges, and more during spikes. A 0603 MLCC
pushed past its rating fails *short*, which shorts the DLC supply.

Fix: 25 V or more on `VIN`. For example `CL21A106KAYNNNE` (10 µF, 25 V, 0805),
or a 1206 part for real derating. C3 (10 µF, 10 V) is fine where it is, on
the 5 V rail.

## 4. High — still no input protection

The old review's §4 still applies. `VIN` comes straight from the connector
with no reverse-polarity block and no clamp. Add an SS34 in series and an
SMBJ18A after it, as on the dash.

## 5. High — 5 V logic between the L9637D and a 3.3 V C6

The L9637D is now on 5 V, which is right, but that makes the logic levels
an issue:

- **`KL_RX` (U3-1 → U1-27).** If RX drives or pulls up to VCC, it puts 5 V on
  a C6 pin that is not 5 V tolerant. Check the datasheet's RX output stage.
  If it reaches VCC, add a 10k/15k divider, or a 3.3 V pull-up if RX is open
  drain.
- **`KL_TX` (U1-26 → U3-4).** Check the TX input's `VIH` at VCC = 5 V. If it
  is 0.7 × VCC (3.5 V), a 3.3 V high does not meet it. In that case fit a
  single-gate level shifter (74LVC1T45, or a BSS138 with pull-ups).

### 5.1 Alternative: TLIN1029 instead of the L9637D

TI's TLIN1029-Q1 (SOIC-8) is a LIN transceiver whose datasheet claims ISO
9141 (K-line) support. It has **no VCC pin**:

- TXD has a VIH of 2.0 V.
- RXD is open drain. Pull it up to 3.3 V with 1–10 kΩ.

So the C6 drives it directly, the 5 V rail goes away, and §5 disappears. Its
pinout nearly fits the L9637D footprint. Pins 1 RXD, 4 TXD, 5 GND, 6 bus and
7 VSUP land in the same places. Pin 3 is NC, so leaving it on the 3.3 V net
is harmless. **Pin 2 is EN** and must be tied high or driven from a GPIO;
floating it means sleep mode.

**The catch: TXD dominant timeout, `tDST` = 20 ms min / 45 typ / 80 max.**
Hold TXD low longer than that and the driver lets go of the bus. The
commonly cited Honda wake-up holds K low for about 70 ms, so on this part
it may be cut off partway through. Two options:

- Add a 2N7002 (or similar) from K to GND, driven by a spare GPIO, for the
  wake-up pulse only. The transceiver handles everything after it.
- Confirm the exact init timing the CRF's ECU needs before choosing.

It also needs the tester-side pull-up: 1 kΩ plus a series diode to VSUP,
per the datasheet's commander-mode circuit. That replaces the §7 question.

## 6. Medium — thermal load on the 78M05

The 78M05 now carries the radio. A C6 with Wi-Fi up and listening draws
roughly 80–100 mA continuously, with TX peaks near 350 mA. At 14.4 V in, the
DPAK dissipates (14.4 − 5) × 0.1 ≈ 1 W on average. On a board this small that
is tens of degrees of rise, under a seat. It has thermal shutdown, so it will
not burn, but it will reset the adapter on a hot day.

A small buck (the LMR51430 already on the dash BOM) removes this problem.
The other way is to keep the 78M05 and duty-cycle the radio (§8).

## 7. Medium — K-line pull-up still unverified

Unchanged from the old review §5. There is no 510 Ω from `KL_SERIAL` to
`VIN`. Check whether the L9637D or the ECU provides one. Bench check: with
the adapter powered and idle, `KL_SERIAL` should sit near battery voltage.

## 8. Antenna — placed correctly

The module's silkscreen runs x 4067.8–4133.1 (EasyEDA units, 10 mil), and the
board edge is at x 4088. The module overhangs the left edge by about 5.1 mm.
Its pads start 0.9 mm inside the edge. So the PCB antenna is entirely off the
board, with no copper beneath it. The GND pours on both layers stop at the
edge. This is one of Espressif's recommended placements.

Two things follow:

- The overhang is the fragile end. The enclosure must hold it, and it must
  not be metal.
- Mount the adapter so the antenna end does not point into the frame or the
  airbox. The DLC is under the seat, the dash is on the bars, and the link
  will be about a metre through plastic and steel. ESP-NOW handles that, but
  orientation is the free part of the link budget.

## 9. Fine as drawn

- USB D+/D− through the USBLC6 to `U1-17`/`U1-18`, as before.
- EN: R4 10k, C9 1 µF, RST button. Boot: R1 on IO8, R2 plus FLASH on IO9.
- `KL_RESET` is a label with no pins on it. It is harmless, but delete it so
  it does not look like a missed connection.
- `U6` is pin 1 GND, pin 3 12 V, pin 4 K. Pin 2 is unused. With no wires to
  the dash, that is all the connector needs.
- ESP32-C6-MINI-1-**H4** (105 °C) on the N4 footprint: same land pattern, and
  the right grade for under a seat.

---

## 10. The dash side — ESP-NOW is not free on the P4

The dash's radio is the Guition module's C6, behind ESP-Hosted. As pinned,
**`esp_hosted` 2.12.8 has no ESP-NOW RPCs.** `esp_wifi_remote` injects an
`esp_now.h` header, but nothing in `esp_hosted` implements `esp_now_init` or
`esp_now_send`. Calling them from the P4 will not link.

What 2.12.8 does have is **peer data transfer**
(`CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER`,
`esp_hosted_send_custom_data()` on the slave). That gives a workable path:

1. **The C6 runs ESP-NOW itself.** This means a change to `demo_p4/c6_cp`,
   which the dash already builds and pushes from `slave_fw`. The C6 receives
   from the adapter's MAC only and forwards each frame to the P4 as a custom
   message. The C6 build id changes, so the "C6 runs exactly the image the
   dash carries" check covers it.
2. **On the P4**, an `EspNowEngine : hal::IEngine` registers the
   custom-message callback. It is a frame decoder with a staleness timeout,
   and nothing above `IEngine` changes.

What this commits the C6 to:

- **Wi-Fi up all the time.** Today it runs only for the update hotspot.
  ESP-NOW needs Wi-Fi started (STA, not associated) on a fixed channel.
- **One channel.** When the update hotspot comes up, the AP must use the
  ESP-NOW channel, or the engine data stops while it is on. Pin the AP
  channel to the ESP-NOW channel.
- **Coexistence with BLE.** One 2.4 GHz radio is time-sliced between NimBLE
  (phone media) and Wi-Fi. Some ESP-NOW frames will be lost. Engine data at
  10–20 Hz with a sequence number and a ~500 ms stale timeout tolerates that.
  Do not design anything that needs every frame.
- **Pairing.** The adapter unicasts to the dash C6's STA MAC, with an ESP-NOW
  LMK for encryption. Both ends learn the other's MAC once (the first
  broadcast heard, in a pairing window like `core/ble/`'s), and keep it in
  NVS.

The frame format, its decode and the stale logic belong in `core/`, so they
are host-tested. Transport on both ends is the thin part.

## 11. Summary

| # | Item | Severity | Fix |
|---|---|---|---|
| 1 | 5 V on C6 3V3 pin | Blocking | 3.3 V LDO for `U1-3` |
| 2 | VBUS / D1 on bike 12 V | Blocking | Own VBUS net; diode-OR into logic rail |
| 3 | C1 rated 10 V on 12 V rail | Blocking | ≥25 V part |
| 4 | No reverse / clamp on `VIN` | High | SS34 + SMBJ18A |
| 5 | L9637D 5 V logic vs C6 3.3 V | High | RX divider, or TLIN1029 (§5.1) |
| 6 | 78M05 ≈1 W with radio on | Medium | Buck, or duty-cycle radio |
| 7 | K-line pull-up unverified | Medium | Datasheet / bench check |
| 8 | Antenna overhang | — | Correct; protect mechanically |
| 10 | No ESP-NOW over ESP-Hosted | Plan | C6 firmware forwards via peer data |

Items 1–3 must be fixed before ordering. Any one of them kills a part on
first power.

---

## 12. Rev (1) netlist — `Sheet_1-copy_2026-10-13(1).net`

The L9637D, the 78M05 and their 5 V rail are gone. U3 is now a TLIN1029DRQ1,
U4 a TLV76133KVUR straight from `VIN` to the 3.3 V rail, and D2 an SS34 from
the connector. The adapter is never on USB and the bike at once (§2).

Module pins, from the ESP32-C6-MINI-1 datasheet: `U1-13` = IO1, `U1-26` = IO20,
`U1-27` = IO21, `U1-17/18` = IO12/IO13 (USB), `U1-22/23` = IO8/IO9.

**Fixed:** §1 (5 V on the module), §3 (C1 is gone), §5 (logic levels: the
TLIN1029 has no VCC pin), §6 (no 78M05), and the USB-only brown-out (TLV761
holds 3.3 V from 5 V VBUS). EN is on IO1 under GPIO control, and floats low
through the TLIN1029's internal pull-down during boot and in deep sleep, so
the transceiver sleeps unless firmware wakes it. VSUP is behind D2, as the
datasheet asks. The TLV761 KVU pinout (1 GND, 2/tab OUT, 3 IN) matches
`U4-1/2/3`.

**Still blocking — D1 on `VIN`.** `D1-5` is still on `VIN`, about 14 V on the
bike, across a ≈6 V internal Zener. Move `D1-5` to `VCC` (3.3 V), or leave it
unconnected. `USB1-1` may stay on `VIN`.

**Check — C4's voltage rating.** The netlist does not carry part numbers. If
C4 is still `CL10A106KP8NNNC` (10 V), it is §3 again. It needs ≥25 V.

**High — no clamp, and the LDO's limit is 20 V.** The TLV761 takes 18 V,
20 V absolute. D2 blocks reverse polarity only, and nothing clamps positive
spikes. An SMBJ18A clamps around 29 V, which is too high for this part. The
choices are a 36–40 V regulator, or a TVS plus series impedance sized to the
TLV761.

**Power, parked:** TLV761 Iq 65 µA typ / 100 max, TLIN1029 asleep 8–12 µA, C6
deep sleep ≈7 µA. That is roughly 80–120 µA, or about 0.15 Ah over 60 days.
It is acceptable, and the LDO is the largest share. **Riding:** linear from
14 V at ~100 mA average is ≈1 W, and the KVU's JEDEC RθJA is 67 °C/W, so
expect ~70 °C of rise on a smaller board. Use ESP-NOW power save on the
sender to bring the average down.

**Missing — how the adapter knows the bike is on.** There is no `VIN` divider
to an ADC pin, and `KL_RX` is on IO21, which cannot wake the chip from deep
sleep (only LP GPIO 0–7 can). Either add a divider from `VIN` to an ADC1 pin
(IO0–IO6; for example 1 MΩ / 150 kΩ, ≈11 µA), or move `KL_RX` to IO0–IO7 to
wake on K-line activity.

**Missing — the Honda wake-up pulse.** The TLIN1029 releases the bus after
20 ms of TXD low (§5.1). Add a 2N7002 from K to GND on a spare GPIO, or
confirm the ECU's init timing first.

**Footprint only — K-line pull-up.** There is no 1 kΩ + diode to `VIN`, only
the TLIN1029's internal 45 kΩ. Leave footprints, DNP, and populate them if
the ECU does not answer.

**2026-10-13, schematic:** `kline-adapter/gen_sch.py` generates both
`kline-adapter-espnow-sch.json` (EasyEDA Std) and `kline-adapter-espnow.net`
from one table and checks the drawing against it. The `VIN` divider (R5, R6,
C11) is dropped: the dash has its own VIN sense, and the ECU reports battery
voltage. Without it, the adapter decides the bike is on by K-line activity
(`KL_RX` is on IO3, which can wake the chip from deep sleep) or by a timed
attempt to talk to the ECU. IO2 is now unused.

## 13. Routed board — `PCB_PCB_kline-adapter-espnow-sch1_2026-10-13.json`

68.0 × 16.5 mm, two layers, 89 tracks (all 0.254 mm), 33 vias (0.62 / 0.31 mm).

**Matches the design.** The exported netlist equals the generated one, except
that R8's ends are swapped, which is harmless for a resistor. Every pad's net
agrees with it. A geometric check of tracks, vias and pads finds every net
fully routed and no shorts. All parts sit inside the outline. The module's
antenna end overhangs the left edge by 4.5 mm, its pads start 1.6 mm inside,
and the only copper near that strip is the GND track along the module's pad
row.

**Fix — clearance.** The VIN track leaving USB1 pin 1 (VBUS) turns diagonally
0.074 mm from shield pad USB1-9. Run it straight down past the shield pad
before turning.

**Fix — no ground plane.** There is no copper pour, so GND, including the
module's return path, is 0.254 mm tracks. Espressif's module guidance is a
solid ground under and around the module. The C6's ~350 mA transmit bursts
through thin ground tracks are a recipe for resets and a poor radio. Pour GND
on both layers, stitch them with vias (especially around U1's GND pads and
U4's tab), and keep the pour out from under the antenna overhang.

**Improve — regulator heat.** U4's tab is GND (pin 6) and has no copper
beyond its pad. Its 31 °C/W rating assumes a JEDEC board with copper. The
GND pour fixes this too, if it connects solidly to the tab.

## 14. Poured board — `PCB_PCB_kline-adapter-espnow-sch1_2026-10-13(1).json`

The netlist is unchanged from §13. GND is poured on both layers over the
whole outline (solid, spoke connections), and each pour is one island tied
to the GND pads. The connectivity check, with the pours included, finds every
net routed, no shorts, and no clearance under 0.15 mm except U3's pads at
0.139 mm, which is legal (the check models U3's oval pads as rectangles, so
the real gap is larger). The USB VIN clearance from §13 is fixed.

**Fix — copper in the antenna keepout.** The module footprint marks the
keepout as x 4030.15–4050.15 (a document-layer region and a silkscreen
line). The board edge is at 4047.56, and both pours reach x 4048.58: about
0.4 mm of copper under the antenna on each layer. Start the pour outline at
x 4050.2, or add a keepout region over that strip.

**Improve — stitching.** There are 8 GND vias in all. Add a row along U1's GND
pads and a few around U4's tab and the connector, so the two pours act as one
ground. Consider a direct (not spoke) connection on U4's tab for heat.

## 15. Revision (2), with BOM

Netlist unchanged. Pad nets match it. Every net is routed, with no shorts and
no clearance problems. The pours now start at x 4061.34, so nothing is in the
antenna keepout (which ends at 4050.15). There are 45 GND stitching vias, up
from 8. U4's tab still joins the pour through spokes.

**BOM — D3 and R9 are listed.** Both are meant to be DNP, but they appear in
the BOM with LCSC numbers (C81598, C21190), so JLCPCB would fit them. Delete
those two rows before uploading, or untick "Add to BOM" on both.

**Check — artwork on the bottom solder mask.** A 10 × 10 mm image (an
SVGNODE) sits on layer 8, the bottom solder mask, at x 4259–4299,
y 3155–3194. Mask artwork opens the mask, so it shows as bare GND pour. Its
right edge is at x 4298.8, and U6's pads end at about 4299.5, so exposed GND
copper sits beside pins 2–4 (12 V and K-line) when the connector is soldered.
If the image was meant for silkscreen, move it to layer 4. If bare copper is
intended, pull it back a millimetre or two from U6.

**Revision (3):** the image is now on layer 4 (bottom silkscreen). The bottom
mask has no artwork openings, and the image's 359 regions moved to the silk
gerber. Its edge is 0.28 mm from U6's pads; JLCPCB clips silkscreen away from
pads anyway. Everything else is unchanged and still passes. Still open: D3
and R9 in the BOM (no new BOM was sent with this revision).

## 16. Independent audit, 2026-10-14 (three reviewers without this document)

Three reviewers each got only the final files: electrical, PCB/manufacturing,
and system. Findings below were spot-checked against the files and code
before being listed here.

**Fix before ordering (board):**
- Vias inside SMD pads: 18 confirmed, including U1-8 (EN), U1-23 (BOOT) and
  U1-26 (KL_TX), whose 0.4 mm module pads are narrower than the 0.62 mm via.
  Also C3, C10, D1-5, D2-1, D3-2, Q1-2, R2, R4, R7-1, both switches and
  U4-2. Move them beside the pads with a short track.
- Two vias 0.19 mm from the board edge: KL_TX gge5763 at (46.0, 16.0) mm and
  KL_WAKE gge6200 at (14.5, 0.5) mm. Move them ≥0.5 mm in. Pours fill to
  0.26 mm from the edge; set the outline clearance to about 0.4 mm.
- A stray circle on Inner2 in the USB1 footprint (gge4778) is why every
  Gerber zip has `Gerber_InnerLayer2.G2`. Delete it.
- One duplicate via on C3-1 (gge5387 / gge5399).
- The BOM still lists D3 and R9. The PCB's flags are right, so the BOM is
  stale. Re-export it.
- The PCB names the switches RST and BOOT; the BOM and CPL call them SW1 and
  SW2. Rename them back so JLCPCB's files match.
- Ground under the module's GND pad column: the pours stop at x = 3.5 mm.
  Extend them to the keepout line (about 0.9 mm) and stitch pads 36–48 and
  pad 49 to the bottom pour.

**Design, worth changing:**
- 3.3 V peak current: the ESP32-C6-MINI-1 peaks at 382 mA (802.11b, 20.5 dBm,
  ESP-NOW's default). The TPS7B82's current limit is 310 mA minimum. Cap TX
  power in firmware (about 13 dBm) and/or add 100–220 µF bulk (total ≤200 µF).
- KL_WAKE on GPIO22 has a 45 kΩ pull-up at reset. Against R8 (100k), the gate
  sits at about 2.3 V, partly turning Q1 on during boot and reset. Use
  R8 = 10 kΩ, or move KL_WAKE to IO14 or an LP GPIO.
- R10 = 10 kΩ costs 330 µA if the TLIN1029 sits in standby (RXD low), which
  is where it powers up. Use 47–100 kΩ; 10400 baud allows it.

**Checked and resolved:** SW1/SW2 use pads 2 and 4. Alps' circuit diagram
for the SKRPACE010 joins 1–2 and 3–4 with the contact between the pairs;
2 and 4 sit 2.2 mm apart on opposite sides. Correct.

**Verify on the bike before connecting:** the DLC pinout. A Honda Europe
sheet gives 1 K-line, 2 GND, 3 VBAT, 4 SCS. A CRF250L community source gives
1 GND, 2 SCS, 3 +12 V, 4 K-line, which is what this board assumes. Measure
which pin is frame ground and which idles near battery voltage with the key
on. Also measure K-line idle voltage, ECU on and off, before fitting R9.

**Dash firmware (not the board):**
- esp_hosted 2.12.8 has no ESP-NOW RPC. The C6 co-processor (`demo_p4/c6_cp`)
  must run ESP-NOW and forward frames over peer data transfer, which is
  already enabled on both sides. Remove `example_peer_data_transfer_init()`,
  which uses handler slots.
- `wifi_service.cpp` sets `WIFI_MODE_AP` when the hotspot starts and calls
  `esp_wifi_stop()` when it stops (grep for them; line numbers drift). Either would kill ESP-NOW. The hotspot must
  switch APSTA ↔ STA on channel 6 instead.

**Adapter firmware rules:** TXD idle-high before EN high (TXD is pulled down
internally). Cycle TLIN EN high then low once after power-up to reach sleep,
since it powers up in standby. Hold IO1, IO20 and IO22 low through deep sleep.
Cap TX power. Sleep on ECU silence. The Honda init (community-sourced): K low
70 ms, high 120–130 ms, `FE 04 72 8C`, then `72 05 00 F0 99`, at 10400 baud.
The ECU sleeps after about 2 s of no traffic.
