# Platform selection for the next board — ESP32-P4 + ESP32-C6

> **Decided.** The P4 was chosen and the board is drawn:
> `p4-board-pin-map.md`, audited in its §6. The status paragraph below is
> as written before that decision.

**Status.** This is a design argument, not a description of anything that
exists. No P4 hardware has been ordered and no P4 code has been written.
The ESP32-S3 board is at fab and is still the platform; see
`docs/bringup.md`. **Nothing here changes what to do next** — the S3 board
gets brought up first, and this document exists so the reasoning survives
the wait.

It records roughly thirty decisions made in one session. Where a decision
was made against a datasheet rather than measured hardware, it says so.

---

## 0. Decisions

| Item | Decision |
|---|---|
| Compute | ESP32-P4 **module** + discrete ESP32-C6-MINI-1 |
| Partition principle | Latency class, enforced by memory domain |
| Supervisor | P4 LP core |
| Link | SDIO, forked `esp_hosted` slave |
| Panel | 5" 800×480 industrial, 1000+ nits, optically bonded, wide-temp |
| Panel interface | LVDS behind an SN65DSI84 bridge |
| Render | Full-frame into a scanned-out framebuffer. Dirty rects retired. |
| Camera | SC2336, native 1080p30 MIPI-CSI, ~120° M12 |
| Recording | H.264 1080p30, fragmented MP4, 10 s fragments, 1–2 s GOP |
| A/V sync | I2S clock is master; video yields |
| Audio | I2S MEMS mic, remote at the airbox, shielded cable |
| Telemetry on video | Side-car file, overlaid in post |
| Storage | SD for video, on-board eMMC for blackbox and config |
| Offload | USB 2.0 HS mass storage primary; on-demand Wi-Fi AP secondary |
| Power | VIN sense on the LP core, hold-up, brownout → finalize |
| Sleep | LP alive briefly after shutdown, then hard cut |
| Enclosure | All-ASA print, aluminium heatsink insert |
| Mounting | Rubber isolators |
| Buttons | Five pod (via C6) + two recovery (direct to P4) + recessed reset |
| Touch | Hooked up if the panel has it. Pit use, not riding use. |
| Sequencing | S3 bring-up first. P4 respin after. |
| Repo | Branch and move wholesale when the time comes. |
| UI retarget | Once a real panel is in hand, not before. |

---

## 1. Why move at all

Not for CPU. The S3 is not compute-bound; it is bound by a wire. A whole
480×320 frame at the ILI9488's 18-bit colour is 460 kB and 92 ms at 40 MHz,
which is 11 fps no matter how fast the drawing is. Every bit of partial-
update machinery in `ui/` exists to work around that one number.

Three things the S3 board cannot do at all:

- **Lap video.** The S3 has no video encoder. Recording means Linux, a
  separate action camera, or nothing.
- **A bright panel worth reading.** Sunlight-readable panels in this size
  are LVDS or high-bandwidth parallel. The S3 can drive neither well.
- **CAN.** Not the chip's fault — the S3 has TWAI — but the board has no
  transceiver. See `docs/kline-can-adapter-netlist-review.md` §1.

An S3 respin with an 8-bit parallel panel and a transceiver would fix the
frame rate and the CAN, cheaply, and is the fallback if the camera
requirement ever softens. It cannot fix the video.

---

## 2. Compute topology

Four execution contexts, not two.

| Context | Clock | Memory | Power domain |
|---|---|---|---|
| P4 HP core 0 | 360 MHz | L2MEM + PSRAM | main |
| P4 HP core 1 | 360 MHz | L2MEM + PSRAM | main |
| P4 LP core | 40 MHz | 32 KB LP SRAM | **separate** |
| C6 | 160 MHz | own SRAM | separate chip |

Plus hardware that does real work without a core: ISP, H.264 encoder, JPEG
codec, PPA, DSI scan-out, SDMMC DMA.

### 2.1 The contention changed shape

On the S3 the threat to the 3 ms tach-edge-to-lamp budget was **scheduling**
— the BLE controller at priority 23 sitting above `ctrl`. On the P4 it is
**AXI/PSRAM bandwidth**. The ISP writes 1080p frames, the encoder reads
them, the DSI controller scans out a framebuffer continuously, SDMMC DMAs
1.5 MB/s to the card. A core executing from PSRAM stalls unpredictably
against that, and no priority saves it.

So the rule that defends the budget is now about **where code lives**:

> Everything reachable from a core-0 task is resident in L2MEM. Nothing on
> core 0 touches PSRAM.

That is checkable. Assert it in CI from the map file. It catches the
regression the day someone adds an allocation to `ctrl`.

### 2.2 Placement

| Context | Owns |
|---|---|
| **HP core 0** — L2MEM-resident | `ctrl` · `imu` · `gnss` |
| **HP core 1** — owns PSRAM | `logic` · `nav` · `render` · camera/ISP · encoder + mux · audio · video→SD · `logger`→eMMC · `keeper` · SDIO |
| **LP core** | VIN sense · brownout → finalize · wake/ignition · post-shutdown flush |
| **C6** | BLE media + TPMS · `core/input/` · pod I2C · touch · neutral |

`logic` and `nav` moved to core 1 because neither is deadline-bound — a late
gear indicator or screen change is invisible. The payoff is that **the whole
3 ms path is now contained in core 0**: tach edge → `engine_sense` capture →
rpm slot → `ctrl` → `shift::pattern()` → lamp FET. No cross-core hop, no
PSRAM access, no dependency on `logic`. Small enough to audit.

At 500 Hz the sampling period alone is 2 ms of the 3 ms budget. Execution
was never the constraint. On a 360 MHz part, run `ctrl` at 1 kHz and buy
back a millisecond for nothing.

### 2.3 The LP core

Its own power domain, so it survives HP sleep. It owns the fast shutdown
path: VIN sense on an ADC, brownout detect, signal the HP side to finalize
the current fMP4 fragment, flush trip state, then cut. This finally closes
the hole in `docs/dash-v1-netlist-review.md` §3 — the current board cannot
tell bike power from USB power and cannot see a shutdown coming.

State handoff needs no new mechanism: the LP core writes into shared LP
SRAM, the HP side reads it as one more `Published<T>`. Mark the region
non-cacheable and the existing seqlock handles tearing.

---

## 3. The C6, and what it costs

The C6 was chosen **discrete** (C6-MINI-1) rather than as an on-module
companion, for one reason: ~20 usable GPIO. That is what lets the
MCP23S17 die. Panel reset becomes a plain P4 GPIO — retiring the "bring the
expander up before the panel" trap in `docs/bringup.md` — and neutral sense,
which is display-only and latency-insensitive, hangs off the C6.

A payoff from design rule 3: because `components/core/` has no ESP headers,
**the C6 links `core/input/` directly**. Debounce and short/long/repeat run
on the C6 as the same code the host tests already cover. Two firmware
images, one core library.

### 3.1 The esp_hosted conflict

`esp_hosted` assumes the C6 is a dumb radio co-processor. Its slave image is
not a framework for running an application beside it. Making the C6 own
buttons, touch and neutral means **forking Espressif's slave firmware** and
adding a custom message path through it. It is an ordinary IDF app and open
source, so this is possible — but it is a fork maintained against upstream,
not a drop-in. That is a known, accepted cost.

The benefit of keeping `esp_hosted` is that TCP/IP and NimBLE stay on the
P4 and the radio appears local, so the §11 BLE design survives essentially
unchanged.

### 3.2 Failure

Watchdog and reset: the P4 holds the C6's reset line and power-cycles it
when the link goes quiet. Guard against a boot loop becoming the failure
mode.

**The residual risk is a dead C6, not a wedged one.** A cracked solder joint
is a real failure on an air-cooled single, and with all input on the C6 that
leaves the dash uncontrollable with no rider-triggerable recovery. Hence the
two recovery buttons wired directly to the P4. They are recovery-only, with
no normal-operation function, so there is no mode confusion with the pod.

Consequence, accepted: manual record start/stop lives on a pod button, so a
dead C6 mid-session means the recording cannot be stopped by hand. The LP
core finalizes it at ignition-off regardless, so nothing is lost.

---

## 4. The panel

5", 800×480, 1000+ nits, optically bonded, wide-temp, LVDS behind an
SN65DSI84.

What actually decided it:

1. **Bonding beats nits.** Air-gapped panels reflect 8–10% of incident
   sunlight off internal interfaces; bonded drops that to 1–2%. Outdoors you
   read *contrast*, not luminance, so a bonded 1000-nit panel beats an
   air-gapped 1500-nit one. Bonding also stops condensation forming between
   layers on a cold morning.
2. **Sunlight blackening is a real failure mode.** An LCD parked in direct
   sun can exceed its isotropic point and go dark and blotchy. Wide-temp
   industrial panels resist it. Phone panels do not.
3. **Dimming range is the forgotten spec.** Night riding wants 1–2%
   brightness. Many high-brightness LED drivers only dim to 10%, and a
   1000-nit panel at 10% is 100 nits — blinding on a dark trail. **Check the
   driver's minimum PWM duty before buying the panel.**
4. **Resolution costs render, not legibility.** At 5" and arm's length,
   800×480 is already ~186 ppi. 720×1280 is ~300 ppi: invisible
   improvement, double the framebuffer and double the fill rate on core 1.
5. **Landscape-native** means the `ui/` retarget is a scale-up, not a
   rotation. Phone panels are portrait; the PPA rotates for free but carries
   an extra buffer.
6. **No AMOLED.** A dash has static chrome in the same pixels for hundreds
   of hours. Burn-in is certain.

Quoted nits are centre-screen at full drive with no cover lens. Add the lens
and lose 10–15%.

Vendors to quote: Riverdi, Newhaven, Winstar, Displaytech, Focus LCDs,
Topway. **The part is not yet chosen.**

### 4.1 What this retires

MIPI-DSI has a real scan-out controller: hardware DMAs the framebuffer to
the panel continuously. Sending pixels stops being a task. `render` simply
draws at 30–60 Hz.

That deletes the dirty-rect tracking, the "anything that moves must be a
widget" rule, and the `test_ui` partial-vs-full equivalence check. It is the
single largest simplification in the move.

Touch, if the panel has it, is a **pit tool**. Capacitive through gloves is
poor and water on the screen causes phantom touches, which in rain is
constant. The five buttons stay.

---

## 5. Camera and recording

**SC2336**, native 1920×1080, MIPI-CSI, ~$8–15, ~120° M12 lens, dash-mounted
forward-facing so the CSI FPC stays under 30 cm and needs no serializer.

The OV2640 was considered and **cannot work**: DVP only, no MIPI, and it
tops out at 1600×1200 stills. Of the sensors with a maintained driver in
`esp_cam_sensor`, those that do 1080p over MIPI are OS02N10, OV2710, OV5645,
SC202CS, SC2336 and OV5640. IMX219 has no driver.

SC2336 wins on one property: **1080p is its native resolution**. On the
OV5640/OV5645, 1080p is a binned crop of a 5 MP sensor, so you read out a
window and lose field of view. Fewer lines to read also means less
rolling-shutter skew, which is the mechanism behind vibration jello on an
air-cooled single. Raw output is fine — the P4's ISP debayers in hardware.

Rolling shutter on a damped mount is the plan; revisit global shutter only
if real footage shows a problem.

### 5.1 Format

H.264 1080p30, **fragmented MP4**. Fragments and GOP are separate knobs:

- **10 s fragments** — low index overhead, few metadata writes.
- **1–2 s GOP** — a fragment must start on a keyframe, but it may contain
  several. GOP = fragment would make scrubbing coarse and let one corrupt
  frame poison ten seconds.

Recording is armed by `core/lap/` and manually overridable. Telemetry is a
**side-car** file, overlaid in post — so the video path is
CSI → ISP → encoder → card with nothing in the middle, no PPA stage, and no
encode-time coupling to the UI.

### 5.2 Audio

I2S MEMS mic, **remote at the airbox**: induction howl and engine note,
shielded from direct wind. A dash-mounted mic records wind roar. BCLK at
48 kHz is ~3 MHz over roughly a metre of shielded cable — no satellite
processor, so cable quality and routing matter.

**The I2S clock is master.** Audio runs free and video timestamps derive
from the sample count. Expect occasional dropped or duplicated frames rather
than audible drift, because listeners notice drift and viewers do not notice
a frame.

---

## 6. Storage and offload

**SD for video, on-board eMMC for blackbox and config.**

The constraint is not bandwidth. 1080p30 at ~12 Mbps is 1.5 MB/s, trivial
for any V30 card. The constraint is a cheap card **disappearing for 200 ms**
during internal garbage collection. Separating the volumes means a stall
cannot hurt the logger, config never risks FAT corruption, and the §9.3 USB
mass storage seam simplifies: the host is only ever handed the SD.

Spec cards by sustained worst-case rating, not headline speed.

Three writers across two volumes now, so `services/storage_service.cpp`
becomes a real ownership model rather than a patch.

### 6.1 Offload

- **USB 2.0 HS mass storage, primary.** ~20–40 MB/s real, so a 20-minute
  session (~1.2 GB) moves in well under a minute.
- **Wi-Fi AP, on demand.** Started from a CONFIG menu, not always on. BLE
  shuts off while it runs — one radio, one antenna.

Wi-Fi has a chain worth knowing: with `esp_hosted` the TCP/IP stack is on
the P4 and the C6 is MAC/PHY, so the path is SD → P4 → SDIO → C6 → air.
Realistic C6 throughput on 20 MHz is 20–40 Mbps, making that same 1.2 GB a
5–8 minute wait. Fine for one clip trackside, tedious for a day's footage.
Hence USB as the bulk path.

---

## 7. Physical

**All-ASA print with an aluminium heatsink insert**, on rubber isolators.

- **A fan was considered and rejected.** A fan needs an air path, an air
  path means the enclosure is not sealed, and a CRF gets pressure-washed and
  ridden through mud. It trades a thermal problem for a water problem and
  loses. The failure case is *parked in the sun*, where ambient is the
  problem and a fan does not help either.
- **ASA, not PLA.** PLA's glass transition is ~60 °C and a dark dash in sun
  reaches 70–80 °C before the backlight is counted; it will sag and take the
  panel bond with it. PETG survives the heat but yellows and embrittles under
  UV. ASA exists because outdoor automotive parts needed it.
- **The backlight, not the SoC, is the dominant heat source** at 1000+ nits,
  and it dumps that heat into the panel — which is also what causes sunlight
  blackening. Thermal-pad the LED driver as well as the module.
- **Plastic is an insulator** (~0.2 W/m·K against aluminium's ~200), so the
  insert must reach outside air to do anything.
- **FDM layer lines leak and layers fatigue in Z.** Gasket groove at every
  mating face, heat-set threaded inserts rather than printed threads, print
  orientation chosen so vibration is not pulling layers apart.
- **Isolators, not a rigid clamp.** The panel is glass and the bars of a
  CRF are not kind to it. Isolation also cuts the high-frequency vibration
  behind rolling-shutter jello.
- **Recess the reset button**, or put it on the back. A board-wide reset on
  the dash face is one accidental press from killing the display and the
  recording mid-lap.
- The camera needs a sealed optical window. Glass or coated polycarbonate —
  acrylic scratches and yellows.

Add a thermistor and a throttle policy in `core/health/`: dim the backlight
first, drop encode quality second.

---

## 8. What this costs the tree

`core/` and its 20 host suites come across untouched. That is the whole
payoff of design rule 3, and it is worth defending.

| Component | Change |
|---|---|
| `core/` | **None.** Plus new modules for recording state and thermal policy. |
| `dash_hal/` | Add `ICamera`, `IRecorder`. |
| `bsp/` | Rewritten. New pin map, DSI/CSI, SDIO, eMMC, DSI84 config. |
| `drivers/` | Rewritten. Panel, camera, encoder, bridge, link are all new. |
| `services/` | `storage_service` becomes three-writer/two-volume. New media services. |
| `ui/` | Retarget to 800×480; delete dirty-rect tracking; relayout 16 screens. `chrome.hpp` should absorb most of it. |
| `sim/` | Two processes with a modelled link. |
| `main/` | Rewritten wiring, across two firmware images. |

### 8.1 The simulator

`sim/` exists so the desktop executes the firmware's decisions rather than a
copy of them. A three-processor system breaks that unless the sim models the
split, so it grows into **two host processes with a simulated SDIO channel**
— injectable latency, severable link. That is more simulator to maintain,
and it is the only way to see a dead C6 before it happens at a track day.

### 8.2 Costs accepted knowingly

1. A maintained fork of Espressif's `esp_hosted` slave firmware.
2. A two-process simulator with a modelled link.
3. Dropped video frames rather than audio drift.

---

## 9. Open

1. **The actual panel part.** Class chosen, part not. This gates the `ui/`
   relayout more than anything else — pick a real part before redrawing
   sixteen screens.
2. **Whether the P4 module exposes DSI on a connector or on castellations.**
   1.5 Gbps differential pairs on 1.27 mm castellated holes is a poor place
   to start. Verify before committing to a module.
3. **PSRAM size on the module variant.** 1080p encode plus DSI framebuffers
   wants 32 MB, not 16 MB.
4. **Framebuffer strategy** — double buffer in PSRAM with vsync is the
   assumption; not yet costed against the encoder's bandwidth.
5. **Render framerate**, 30 or 60.
6. **Thermal load**, actually calculated rather than argued.
7. **Second CAN channel.** The P4 has one TWAI. Bike CAN plus an aftermarket
   ECU would need a TCAN4550 or MCP2515 on SPI.
8. **New `Frame` fields** for camera, recording and thermal state.

---

## 10. Sequencing

**The S3 board is brought up first.** It is the only path to validating the
drivers, the panel stack, GPS, IMU and the real-time story against real
hardware, and everything learned transfers. Respinning to the P4 before that
would leave two untested boards and no reference to bisect against — when
something did not work, there would be no way to tell which layer was lying.

When the respin comes, the codebase **branches and moves wholesale**: P4
becomes the trunk, S3 stays on a branch. Not a dual-target build matrix.

`ui/` retargets **once a real panel is in hand.** Layout decisions made
against a spec sheet get redone the first time you see 1000 nits at arm's
length in daylight.

Read `docs/bringup.md` §0 before applying power. Nothing in this document
changes that.
