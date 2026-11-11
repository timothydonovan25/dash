# Competitor survey: dashboards, loggers and rider computers

Compiled 2026-11-10 from web searches. 62 products and projects in nine
categories, from dirt-bike trip computers to superbike OEM clusters.

**Read this first.**

- Almost every figure here comes from retailer listings and search
  summaries, not manufacturer datasheets. Where sources disagreed, both
  values are given. Check a number against the maker's manual before
  relying on it.
- "Not found" means the search returned nothing, not that the feature
  does not exist.
- Prices are whatever the listings showed, in their own currencies.
- Section 11 lists what was searched for and not found.

## Contents

1. Race dash-loggers (display + logging)
2. GPS lap timers with a display
3. Loggers without a display
4. Off-road trail computers
5. Street and custom gauges
6. Shift lights and gear indicators
7. Rally tripmasters and roadbook gear
8. Navigation units and rider tablets
9. OEM clusters and factory loggers
10. DIY and open-source projects
11. Searched, not found
12. How this dash compares
13. Sources

---

## 1. Race dash-loggers

### 1. AiM MXm

- **Display:** 268×128 px LCD, configurable RGB backlight, ambient light
  sensor.
- **Sensors:** integrated GPS; internal 3-axis ±5 g accelerometer and
  3-axis gyro (official sheet; one retailer says "9-axis").
- **Inputs:** coil RPM input, two speed inputs, digital inputs and
  outputs, two high-side outputs. ECU over CAN, RS232 or K-Line.
- **Data:** 4 GB internal memory.
- **Capabilities:** 5 shift-light LEDs and 2 alarm LEDs; Wi-Fi download to
  Race Studio 3.
- **Physical:** 137 × 88 × 30–32 mm; 330 g or 390 g and IP65 or IP67
  depending on the listing (official sheet points to 330 g, IP65). 37-pin
  motorsport connector plus 4-pin power.
- **Price:** about £1,025 to $1,449.

### 2. AiM MXS 1.3 Strada

- **Display:** 5" TFT, 800×480.
- **Sensors:** none built in that the sources confirm. A GPS module is
  listed as an option; compatibility not clarified.
- **Inputs:** 8 configurable analog channels and 2 digital (optional
  23-pin harness) for temperatures, pressures, potentiometers. ECU over
  CAN, RS232 or K-Line (OBDII). 14-pin and 23-pin AMP connectors.
- **Data:** **no internal logger.** It is the MXS 1.3 minus logging; add
  an AiM ECULog on the expansion CAN, or buy the MXS Logger version.
- **Capabilities:** RGB shift lights with a max RPM per gear (one listing
  says ten); 6 configurable RGB alarm LEDs (official sheet).
- **Physical:** 480 g, 400 mA, IP67 (AiM) or IP65 (a retailer).

### 3. AiM Solo 2

- **Display:** 238×99 px LCD, up to 5 freely configurable pages,
  backlight in 7 or 8 colours.
- **Sensors:** 25 Hz GPS on four constellations (GPS, GLONASS, BeiDou,
  Galileo), position accuracy under 0.5 m.
- **Data:** lap times to 1/100 s, splits, 4 GB internal memory.
- **Capabilities:** predictive lap time, instant speed, 10 configurable
  RGB LEDs compared against best lap, thousands of preloaded tracks
  recognised at power-on, Wi-Fi download.
- **Physical:** IP67 (one listing IP65), 249 g with battery. Optional
  motocross bar-pad mount with shock protection.
- **Price:** £350 for the motocross version.

### 4. AiM Solo 2 DL

- Everything in the Solo 2, plus:
- **Inputs:** ECU over CAN, RS232 or K-Line, or a coil signal, "to
  thousands of ECUs"; OBDII harness option.
- **Data:** engine channels logged beside GPS.
- **Price:** $849 (US), £756 (UK).

### 5. Starlane Davinci-II R / S

- **Display:** 5" colour TFT.
- **Sensors:** GPS (listings say triple-constellation; a related model
  is 10 Hz GPS+GLONASS). Inertial Pack, standard on the R.
- **Inputs:** RPM and gear; the R adds 4 analog inputs at 500 Hz
  (throttle, suspension, brake pressure). Wireless WID sensor modules.
- **Data:** lap time with intermediates, GPS speed, GPS trajectories,
  RPM, gear. Analysis in MAAT software.
- **Capabilities:** lean angle, lateral and longitudinal g. Lean is
  computed about the axis through the combined bike + rider centre of
  gravity, accounting for rider position. 1,000 preloaded tracks,
  1/100 s lap timing. Download over USB, Bluetooth or Wi-Fi. Water
  resistant. Bike-specific plug kits sold separately.
- **Price:** roughly $1,899 to £1,621.
- **Note:** retailer copy mixes R, S and X-series details.

### 6. Starlane Corsaro-II R / Pro

- **Display:** touch screen.
- **Sensors:** GPS.
- **Inputs:** RPM, water temperature, exhaust temperature, speed
  (per-lap values, depending on configuration). Wireless expansion with
  optional WID and RID modules.
- **Data:** 999 laps in 99 sessions; analysis in Digirace-MMX.
- **Capabilities:** Bluetooth 4.0 for settings, lap triggers and
  download; Wi-Fi on the Pro (one listing also claims Wi-Fi on the R,
  unconfirmed). Hour meter.

### 7. I2M Chrome Pro 2

- **Display:** 5" colour, 800×480.
- **Sensors:** GPS receiver; TPMS (antenna plus two receivers, 8.5 mm or
  11.5 mm valves); Bosch LSU 4.9 lambda sensor.
- **Inputs:** CAN bus acquisition (many more channels than the Chrome
  Lite); video input.
- **Data:** full data logger.
- **Capabilities:** dashboard, GPS lap timer, TPMS, lambda analysis.
  Controls a GoPro Hero 4–7 over Wi-Fi and starts recording in sync with
  the data. Plug-and-play loom per bike. Sold for racing; may not keep
  OEM street functions.
- **Price:** about $2,489 to $3,384.

### 8. I2M DataLog Pro

- **Display:** none.
- **Capabilities:** same electronics and most of the same software as the
  Chrome Pro 2, as a cheaper logger without a dash. A Euro5 adapter
  version exists.

### 9. MoTeC C125 (marked obsolete by MoTeC)

- **Display:** 5" colour TFT, 800×480, anti-reflective and
  sunlight-readable.
- **Inputs:** CAN to any current MoTeC ECU and other CAN devices; RS232
  for older M4/M48/M8 ECUs. A retailer lists dual CAN and a composite
  video input.
- **Data:** logging is a paid upgrade: 120 MB at up to 500 samples/s; a
  later USB option reportedly logs up to 32 GB.
- **Capabilities:** 10 full-colour LEDs for shift lights and warnings;
  colour, function and intensity of each is programmable.
- **Not found:** the list of CAN channels.

### 10. PZRacing Start Plus (ST400-P)

- **Display:** multifunction dashboard, the newest of the Start range.
- **Sensors:** GPS (one listing says 50 Hz).
- **Data:** data acquisition; download over Wi-Fi or USB cable.
- **Capabilities:** built-in 2000 mA lithium battery charged over USB-C.

---

## 2. GPS lap timers with a display

### 11. PZRacing Start Next (ST400-N)

- **Sensors:** 50 Hz GPS.
- **Data:** GPS trajectories, speed, direction, acceleration and lean
  angle, reviewed on a PC.
- **Capabilities:** **lean angle estimated by algorithm from GPS alone**,
  with no sensor on the bike. The Wi-Fi version detects the circuit
  automatically from a global database with no button presses.

### 12. PZRacing Start Basic (ST400-B)

- **Sensors:** the same 50 Hz GPS receiver.
- **Data:** none stored; lap times and trajectories only.
- **Note:** the ST400 code appears on several different products.

### 13. Starlane Stealth GPS-4 (and GPS-4 Lite)

- **Display:** six-digit display, 1/100 s chronometer.
- **Sensors:** GPS; inertial pack is an option.
- **Data:** session telemetry; export for analysis and video overlay.
- **Capabilities:** download to a USB pen drive with no computer at the
  track. Billed as the lightest and smallest GPS motorcycle lap timer.
  Bluetooth not confirmed.

### 14. Alfano 6 (1T / 2T)

- **Display:** 168×128 px, backlight in 3 levels, customisable layout and
  background colour.
- **Sensors:** GPS, GLONASS and Galileo; integrated antenna; 10 Hz.
- **Inputs:** RPM lead; gear shown via the speed sensor; compatible with
  Pro3 Evo sensors. One listing gives the available inputs as 3 position,
  1 valve, 4 temperature, 4 speed, 1 analog, 1 lambda.
- **Data:** lap times, speed, trajectory; Bluetooth download to tablet,
  phone or PC.
- **Capabilities:** over 2,550 tracks in memory, recognised
  automatically, or a new one created from GPS. Rechargeable lithium
  battery.
- **Physical:** IP56, 136 × 88 × 33 mm, 435 g. Kart and MiniGP are its
  main markets.

### 15. Alfano 7 (1T)

- **Sensors:** 25 Hz GPS (the stated improvement over the Alfano 6).
- **Inputs:** one temperature input on the 1T.
- Other details not found.

### 16. GET (Athena) MD60 LOG

- **Sensors:** 5 Hz GPS. No external sensors or transponders.
- **Data:** up to ten sessions of up to 99 laps; a data-logging mode
  records more and drains the batteries much faster. Instantaneous speed,
  acceleration and braking phases.
- **Capabilities:** runs on regular batteries; finish line and splits set
  by the rider; download to GET's GATE software (Windows only). Sold for
  motocross, supermoto, enduro, karting.
- **Physical:** 108 × 56 × 30 mm, 200 g, mineral-filled polyamide case.
- **Price:** $399 (US review); ¥63,000 in Japan. RevZilla lists it as a
  closeout, no longer available.

### 17. Qstarz LT-9000S

- **Display:** 2.3" colour.
- **Sensors:** 25 Hz GPS.
- **Physical:** IPX5.

### 18. Freelap MX Timer

- **Sensors:** none on GPS; a transmitter-based timer kit.
- **Data:** lap times and best lap, 2/100 s accuracy.
- **Physical:** back-lit display, water resistant.

---

## 3. Loggers without a display

### 19. RaceBox Mini S

- **Sensors:** 25 Hz GPS, GLONASS, Galileo, BeiDou and SBAS;
  accelerometer sampled internally at 1 kHz, ±8 g. A gyroscope is not
  clearly stated on the official listing.
- **Data:** onboard storage, 130 min at 25 Hz or 325 min at 10 Hz.
- **Capabilities:** standalone (not phone-tethered); lap timer and drag
  meter; ships with mounts for bikes.

### 20. RaceBox Micro

- **Sensors:** u-blox SAM-M10Q GNSS, 25 Hz; accelerometer and gyroscope
  (retailer wording).
- **Data:** same storage as the Mini S (figures from preproduction
  units).
- **Capabilities:** a bare 15 g board for DIY builds; RaceBox protocol
  **and NMEA over BLE**; "all the features of the Mini S".
- **Price:** $119 on pre-order at one retailer.

### 21. LITPro (app + HDGPS)

- **Sensors:** LITPro HDGPS at 20 Hz, or RaceBox Micro / Mini S, Garmin,
  Polar, Apple Watch, XGPS160, or a phone (1 Hz).
- **Data:** lap times, segment times, personal records, consistency.
  Vendor claims lap and segment times within 1/20 s of official AMA
  results (unverified).
- **Capabilities:** **automatic jump detection**: airtime, g, launch
  speed, landing speed; total and longest airtime per session. Gate-drop
  analysis, corner analysis, acceleration/deceleration breakdown, lap
  comparison, leaderboards, GPS playback colour-coded by speed.
- **Note:** forum users say a GPS watch is accurate enough for lap times;
  the dedicated unit buys the deeper analysis.

### 22. Autosport Labs RaceCapture/Pro MK3 (legacy; MK4 exists)

- **Sensors:** 6-axis IMU (3 accelerometer + 3 gyro channels, real not
  synthesised); quad-constellation GNSS up to 25 Hz.
- **Inputs:** 9 analog channels (8 external + 1 battery), 0–5 V; CAN with
  OBDII and custom channel mapping (MoTeC, Link, AEM, Megasquirt).
- **Capabilities:** lap timer, data logger, real-time telemetry.
- **Not confirmed:** an open-source licence for hardware or app.

### 23. Autosport Labs RaceCapture/Track MK3 (replaced by MK4)

- **Inputs:** 2 CAN channels at 125 k / 250 k / 500 k / 1 M baud; OBDII
  PID and custom CAN mapping; a retailer lists 100 CAN channels.
- **Capabilities:** cellular telemetry broadcast at 1, 5 or 10 Hz. M8
  connectors for power, CAN1 and CAN2 expansion.

### 24. Racelogic VBOX Sport (discontinued)

- **Sensors:** 25 Hz GPS.
- **Capabilities:** waterproof, battery powered, over 6 hours; Bluetooth
  to iOS; performance testing and lap timing; a bar mount was sold.

### 25. Racelogic Performance Box Touch

- **Display:** 4.3" colour touchscreen (so it is a display unit, listed
  here with its family).
- **Sensors:** 25 Hz GPS.
- **Capabilities:** no phone needed; live slope correction for
  acceleration and braking results; up to 6 h with the clip-on battery;
  analysis in Circuit Tools and VBOX Test Suite.
- **Price:** £575 to £975 + VAT.

### 26. Racelogic VBOX Touch Motorsport

- **Display:** 4.3" colour touchscreen.
- **Sensors:** 25 Hz GPS.
- **Capabilities:** runs multiple applications on the same hardware.
- **Price:** around £2,100.

---

## 4. Off-road trail computers

### 27. Trail Tech Voyager Pro

- **Display:** 4" glove-friendly touchscreen, 480×272, IP67.
- **Sensors:** GPS; Trail Tech vehicle sensors for engine temperature,
  RPM (tach sensor in the universal kit), battery voltage,
  speed/distance. Speed from GPS or a wheel sensor.
- **Data:** tracks, routes, waypoints; reads and records GPX.
- **Capabilities:** preloaded North American topo maps. **Buddy
  tracking**: up to 20 riders on the map over sub-GHz radio with no cell
  service, 0.5–1.5 miles by terrain; push-button emergency beacon to the
  group. Enabled and legal only in the Americas and Australia / New
  Zealand. Internal 3.7 V 3100 mAh battery.
- **Price:** about $597 to $1,300 by kit and retailer. Kits: universal
  922-123; KTM/Husqvarna/GasGas 922-128.

### 28. Trail Tech Voyager Pro4 (released July 2026)

- **Display:** 4.3" WQVGA, **1000 nit**.
- **Capabilities:** Bluetooth 5.1; microSD up to 64 GB; "Buddy Tails"
  breadcrumb trail of each rider's recent movement; works with Polaris
  Ride Command Group Ride vehicles.

### 29. Trail Tech Vapor

- **Display:** large RPM graph and speed readout.
- **Sensors:** wheel speed, tach, engine temperature (10 mm sensor on
  the air-cooled kit; water probe or in-line coolant sensor on others),
  ambient temperature.
- **Data:** speed, RPM, engine and ambient temperature, resettable
  distance, clock, ride time, odometer.
- **Capabilities:** LEDs blink at the shift point and on over-temperature.

### 30. Trail Tech Endurance II

- **Sensors:** wheel sensor (varies by kit). No RPM or temperature.
- **Data:** speed, max and average speed, two trip distances, odometer,
  ride time, clock.
- **Capabilities:** maintenance reminder by time or mileage; full-time
  backlight; hardwired power.

### 31. Trail Tech Striker

- Sold as "Striker Kit" and "Striker Computer Kit" with a fin
  (cylinder-head) temperature sensor. No feature details found.

---

## 5. Street and custom gauges

### 32. Koso RX-4

- **Display:** 4.3" colour TFT; backlight blue, green, orange or white on
  one listing.
- **Sensors:** supplied sensor wire set and a separate temperature
  sensor fitted where you want to measure.
- **Data:** speed, odometer, trip, hour meter, voltage, clock,
  stopwatch, low-fuel mileage.
- **Capabilities:** tachometer with shift light, indicator lamps. The
  universal Bluetooth version pairs with iOS for call notification and
  music. Bike-specific versions exist (MT-07, MT-09, XSR 700/900,
  Hayabusa 2008–2020). Operating range −4 °F to 185 °F.

### 33. Motogadget Motoscope Pro

- **Display:** self-lit LED matrix (734 or 718 LEDs by source),
  brightness automatic for direct sunlight; scalable LED band as the rev
  counter with a drag (peak-hold) indicator; user-configurable main and
  secondary fields.
- **Inputs:** full CAN interface; one-wire bus and Breakout Boxes for
  expansion; stainless IP67 speed sensor in some kits; four indicator
  lights assignable to inputs.
- **Physical:** 109 × 47 × 11 mm, about 100 g with cable, 9–15 V, 200 mA
  max, 0.4 mA standby, −20 to +80 °C, two M3 mounting holes.
- **Note:** the touch sensor is discontinued; a blog describes a
  three-button interface.

### 34. Acewell ACE-6000 series

- **Display:** needle tachometer with digital speed and rev readouts;
  6 to 10 LED indicators. Tach range by family: 64xx 9,000 rpm, 65xx
  12,000, 66xx 15,000, 67xx 20,000.
- **Data:** speed, rpm, clock, odometer, trip, temperature, voltage,
  fuel gauge, acceleration timers.
- **Capabilities:** warning lamps; ACE-6654 adds lap timing and is ADR
  approved for road use.
- **Price:** around €260.

### 35. Daytona Velona (60 mm, 80 mm "W", Velona 2)

- **Display:** analog speed and rev counter (W) with an LCD.
- **Data:** odometer, trip meters, voltmeter, max speed.
- **Inputs:** digital speed signal from the bike; universal sensor for
  bikes without one. 9–16 V.
- **Capabilities:** indicator lamps; ECE-R10, TÜV, ABE approvals on the
  60 mm.
- **Price:** about €130 to €235.

### 36. Dakota Digital MLX / 5000 series

- **Display:** high-brightness readout, readable in direct sunlight; 30
  lighting colours (MLX-5200-K).
- **Data:** 0–255 mph/kph speed, timers, service mileage reminder.
- **Capabilities:** MLX-9214 has a customisable display and Bluetooth.
  Mostly Harley-Davidson and custom fitment; MLX-5200-K not recommended
  for CAN bus.

---

## 6. Shift lights and gear indicators

### 37. HealTech GIpro DS (G2)

- **Inputs:** gear position read from the ECU through the diagnostic
  port. No buttons, no programming wires.
- **Capabilities:** programmable shift light and high-speed warning;
  sampling period adjustable in 10 steps (0–9, default 3).
- **Physical:** 20 × 30 × 13 mm, IP68, −40 to +80 °C, reverse-polarity
  and transient protection on every lead. Display colours on related
  models: white, red, blue, green, yellow.

### 38. HealTech Shift Light Pro (SLP-U01)

- **Capabilities:** 3-stage programmable shift light, or an optimal
  rev-range indicator for economy. One fully programmable RGB power LED.
  **Set up over Bluetooth** from Android or a Windows PC; saved profiles
  switch in a few taps.
- **Physical:** 30 × 20 × 13 mm, waterproof, 2-year warranty.
- **Price:** about US$145; AU$180.

### 39. Motogadget SureShift

- **Display:** 37 ultra-bright red LEDs, brightness automatic, normal or
  inverse digit.
- **Capabilities:** gear indicator with two shift-light levels at freely
  chosen RPM. Includes a speedometer sensor kit.

### 40. Koso gear indicator with shift light

- **Capabilities:** gear display plus a programmable shift light. Not
  suitable for Harley-Davidson or the Honda CB 650 FA.

### 41. Daytona LCD gear position indicator

- **Capabilities:** selectable top gear (4 to 8), programmable,
  waterproof, 12 V.

Budget ECU-plug gear indicators (Idea and unbranded) also exist.
Reviews report a slight display delay, wrong gear on some bikes, and
calibration that has to be done while riding.

---

## 7. Rally tripmasters and roadbook gear

### 42. ICO Racing Rallye MAX-G

- **Sensors:** GPS only; receiver wants a clear sky view and a near
  horizontal mount.
- **Data:** rally odometer, CAP (course over ground, degrees), speed,
  resettable max speed, GPS clock adjusted to time zone.
- **Capabilities:** AutoCal (on by default) corrects the odometer against
  less accurate route measurements; settings persist through sleep after
  two hours idle; 3-button remote; waterproof; drop-in for the Rallye VRL
  and CAP. Reviewers suggest it as a backup to a wheel-sensor odometer.
- **Physical:** 93 × 47 × 30 mm, 120 g.
- **Price:** about £366 ex VAT; €459–499.

### 43. RNS TripMaster GFX v2 Pro

- **Display:** **2.7" transflective** graphics LCD, 400×240, high
  contrast.
- **Sensors:** built-in GPS antenna and receiver; optional wheel sensor,
  including a "mixed" GPS + wheel mode.
- **Capabilities:** repeater for a Garmin GPS (waypoint arrows and data,
  always or within 500 m / 1000 m). Related models: GFX v2 Standard,
  Tripmaster XL Pro, XL Rally, XL3.

### 44. F2R RB850 roadbook kit

- **Capabilities:** electric paper roadbook holder (roll up to 55 mm
  thick) with LED lighting. The Amateur kit bundles an ICO Rallye MAX-G,
  power box and remote. The F2R race add-on bracket carries combinations
  of ICO or RNS tripmasters and a SpeedoCap (for example two trips and a
  speedo).

---

## 8. Navigation units and rider tablets

### 45. Thork Racing DMD-T865X

- **Display:** 8", high brightness (1300 nit on one listing; "over 1000"
  from Thork).
- **Sensors:** high-precision GPS.
- **Capabilities:** Android 14, IP67, 128 GB + microSD, 5 GHz Wi-Fi,
  Bluetooth, LTE. Runs straight from 12 V. Wired and Bluetooth handlebar
  controllers that also drive 20+ other navigation apps. Lifetime DMD2
  licence with worldwide road and off-road maps; dashboard shows speed,
  fuel level and more.
- **Price:** £780 (X), £648 (T865); US $799.95.
- **Not found:** OBD or sensor inputs.

### 46. Carpe Iter Pad (v4c is the newest with specs found)

- **Display:** 7", 1920×1080, **2600 nit**.
- **Capabilities:** Android 13 in a rally-proven chassis; handlebar
  controller; HUD companion software with a PDF roadbook reader, external
  tripmaster control and odometer; DMD2 preinstalled on some listings.
  Bluetooth OBD dongle support for CAN-bus bikes described as a plan, not
  a shipped feature. The Caretta is a related model.

### 47. Garmin zūmo XT2

- **Display:** 6".
- **Capabilities:** street and mixed riding, large route-planning maps;
  group ride through the Tread app (up to 20 riders on screen, leader
  shares a ride ID); pairs with inReach Mini 2 for satellite messaging;
  up to 7 h battery, about 5 h at full backlight.
- **Price:** $599.99 MSRP.
- **Not found:** pitch and roll gauges.

### 48. Garmin Tread 2

- **Display:** 6" model with similar battery life.
- **Sensors:** 10 Hz GNSS; port for an external GPS antenna.
- **Capabilities:** off-road bias; MIL-STD-810 drop rating; preloaded US
  public land boundaries; group ride and inReach as on the XT2.

### 49. Beeline Moto II

- **Display:** 1.45" round IPS TFT, 412×412, anti-reflective and
  hydrophobic.
- **Sensors:** accelerometer, gyroscope, magnetometer. Position comes
  from the phone.
- **Capabilities:** turn-by-turn with ETA and speed, or a simple arrow;
  Route, Tracking and Compass modes; Fast and Fun routing; GPX import and
  export; speed-limit and camera alerts where available; RockerTop
  glove buttons. Needs a phone and the Beeline app; some features
  (spoken instructions, round-trip generator) may need a Plus
  subscription.
- **Physical:** 600 mAh, 14 h riding, months standby, USB-C and pogo pins
  (charge while riding), IP67, 53 mm × 20.5 mm, 42 g (60 g Metal).

### 50. Chigee AIO-6 (Max and LTE)

- **Display:** 6", 2000 or 2300 nit by source.
- **Sensors (mostly optional):** TPMS (external or internal sensors, tyre
  pressure and temperature); rear radar for blind-spot detection and rear
  collision warning; front and rear 1080p cameras; Bluetooth OBD module;
  CGRC LIN module for BMW (a CGRC CAN module is named but undocumented).
- **Data:** dashcam recordings with GPS speed and braking overlays; OBD
  data overlaid and synced with video.
- **Capabilities:** phone mirroring display; Insta360 integration; the
  LTE adds 4G for live tracking and emergency SOS. Launched on
  Kickstarter.

---

## 9. OEM clusters and factory loggers

### 51. BMW M 1000 RR / S 1000 RR

- **Display:** 6.5" TFT, shared with the S 1000 RR, with an M start
  animation.
- **Capabilities:** **M GPS laptrigger and M GPS data logger are an
  activation code** delivered through the cluster's OBD interface; the
  GPS hardware is bought separately. Data for about 300 racetracks. The M
  Competition package bundles the laptrigger code.
- **History:** a factory-fit 2D data recording system was offered for
  the S 1000 RR.
- **Aftermarket:** PZRacing's B2-Tronic plugs into the stock dash, adds
  GPS lap timing and calculates lean angle per corner (M 1000 RR
  2020–24).
- **Not confirmed:** a factory lean-angle readout in these sources.

### 52. Ducati Panigale V4 (2025 on)

- **Display:** 6.9" full TFT, 8:3, 1280×480.
- **Sensors:** six-axis inertial platform with the Ducati Vehicle
  Observer algorithm; feeds cornering ABS by lean angle.
- **Capabilities:** "ready for" the Ducati Data Logger (DDL), which
  shows lap and sector times and holds finish lines for more than 500
  circuits, and for the Ducati Multimedia System. The DDL is not
  standard.
- **Not confirmed:** a g-meter or lean readout on the dash; a dedicated
  track display mode.

### 53. Yamaha YZF-R1M CCU + Y-TRAC

- **Sensors:** CCU (Communication Control Unit) with a GPS antenna.
- **Data:** logged ride data with GPS course mapping and automatic lap
  timing; a record line can be set before a session.
- **Capabilities:** data pulled over Wi-Fi into the Y-TRAC app (Android
  first, then iOS, 2015) and overlaid on the track map; **bike settings
  edited in the app and uploaded back to the bike**. Optional on the
  standard R1.

### 54. KTM V80 / H88 TFT + CCU3.0

- **Display:** touchscreen TFT, vertical 8" (V80) or horizontal 8.8"
  (H88); shape said to be inspired by rally roadbooks.
- **Capabilities:** connectivity unit with eSIM, GPS, Bluetooth and
  Wi-Fi; offline map navigation on the bike with no phone; new
  switchgear; dealer updates first, over-the-air planned. Launched on the
  1390 Super Adventure and 1390 Super Duke GT.
- **Not found:** anything for the 450 Rally or EXC.

---

## 10. DIY and open-source projects

### 55. blackbox (jctoledo)

- ESP32-C3 firmware in Rust. Fuses GPS (NEO-6M, or NEO-M9N at 25 Hz) and
  a WT901 IMU with an **extended Kalman filter** for position, velocity
  and acceleration; serves a dashboard over Wi-Fi. Track-day logger, not
  a cluster.

### 56. Honda-Motorcycle-Dash (sophienyaa)

- Alpha-stage dash that reads data from Honda ECUs. The closest project
  to the K-line adapter work here.

### 57. Honda dash write-up (jpralves.net)

- ESP32 (Node32S for development, ESP32-WROOM for the build), 2.8"
  ILI9341 SPI TFT, serial GPS module.

### 58. IceNav v3 (jgauchia)

- ESP32-WROVER, **ILI9488** TFT, GPS, MPU6050 or MPU9250 IMU, LVGL over
  LovyanGFX. A GPS navigator with offline maps.

### 59. bike-computer-32 (lspr98)

- ESP32-C3 bicycle computer: offline OSM maps, GPX track rendering,
  multi-constellation GNSS.

### 60. Zero-Dashboard (remmie1972)

- Replacement dashboard for Zero electric motorcycles: TFT, CAN bus,
  Adafruit ESP32.

### 61. mazduino-dash-canbus (amrikarisma)

- ESP32, CAN bus, TFT. No description in its listing.

### 62. megasquirt-dash-display (MWP)

- Dash for Megasquirt ECUs on an ESP32 with an EVE2 display.

Also returned, with no detail: MotoMate (WRansohoff, GitHub), a GPS
bicycle computer with SD logging on Printables, and a MotoTracker ESP32 +
SIM7000G GPS tracker.

---

## 11. Searched, not found

- Koso DB-EX and RXF
- Qstarz LT-8000GT (search was rate-limited)
- Vortex, CrossBox and XRacer motocross timers (CrossBox appears only as
  a forum thread title, "LITPro vs CrossBox")
- 2D Datarecording BigDash / MiniDash
- F2R RB750
- KTM 450 Rally and EXC instruments
- Carpe Iter Pad v5
- Ecliptech SHIFT-I
- Trail Tech Striker's feature list
- Alfano 7 beyond its 25 Hz GPS

Not searched at all: Honda's own CRF instruments, Koso and Sigma hour
meters, RealDash and other phone-app dashes, Garmin Montana, TripLTek and
other rugged tablets, Carpuride and similar CarPlay screens, Polaris Ride
Command, any forum or Reddit build threads.

---

## 12. How this dash compares

From this repo's own docs: 3.5" 480×320 panel, GPS, IMU, shift lamps,
SD blackbox for RaceRender, BLE and Wi-Fi through the C6, offline OSM
routing, inductive tach, wheel sensors, a button pod.

**What nothing else in the survey combines.** No single product pairs a
trail computer's inputs (inductive tach, wheel speed, neutral) with a
race logger's IMU and shift lights and with on-device offline routing.
The Voyager Pro has maps and the inputs but no IMU or logging for video.
The AiM MXm has the IMU, logging and shift lights but no maps. Tablets
have maps and nothing else.

**Where competitors are ahead.**

| Area | Competitors | This dash |
|---|---|---|
| GPS rate | 10–50 Hz (AiM 25, PZRacing 50, RaceBox 25) | stuck at 1 Hz (bringup) |
| Display brightness | 1000–2600 nit, or transflective (RNS) | 3.5" IPS; transflective panel on order |
| ECU data | CAN / RS232 / K-Line on AiM, HealTech, I2M | K-line adapter not ordered |
| Lap timing | preloaded track databases, predictive lap | not in this survey's scope of the docs |
| Analysis software | Race Studio 3, MAAT, Circuit Tools, LITPro | RaceRender CSV |

**Ideas worth a look.**

- LITPro's jump detection (airtime, launch and landing speed) is pure
  GPS + IMU post-processing, and suits a CRF250F better than lap timing.
- PZRacing estimates lean angle from GPS alone; Starlane corrects lean
  for rider position. Both are checks against an IMU-derived value.
- RNS's "mixed" GPS + wheel odometer and ICO's AutoCal are the rally
  world's versions of the speed fusion in `docs/fusion.md`.
- Trail Tech's buddy tracking over sub-GHz radio is the one off-road
  feature with no equivalent here.
- I2M triggers a GoPro over Wi-Fi so video and data start together,
  which removes the manual time alignment RaceRender needs.
- RaceBox Micro speaks NMEA over BLE, so third-party apps can use it as
  a GPS source.

---

## 13. Sources

Race dash-loggers and lap timers

- AiM MXm: https://aimsports.com/download/technical-sheets/aim_mxm.pdf ·
  https://www.aimshop.com/fr/products/aim-mxm-motorcycle-data-logging-dash-with-gps ·
  https://xtramotorsport.com/product/aim-mxm-compact-dash-logger/ ·
  https://hhrperformance.com/i-31638923-aim-mxmmotorcycle-data-logging-dash-with-gps.html ·
  https://www.bimmerworld.com/AiM-MXm-Compact-Dash-Logger.html ·
  https://discoveryparts.com/products/aim-sports-mxm-compact-dash-logger
- AiM MXS 1.3 Strada: https://aim-sportline.com/download/technical-sheets/aim_mxs_1.3_strada_102.pdf ·
  https://product41.com/products/aim-mxs-1-3-strada ·
  https://www.aim-sportline.com/en/products/mxs-strada ·
  https://discoveryparts.com/products/aim-sports-mxs-1-3-strada-5-color-tft-dash ·
  https://xtramotorsport.com/product/aim-mxs-strada/
- AiM Solo 2 / DL: https://www.aimshop.com/products/aim-solo-2-dl-motocross-track-day-lap-timer ·
  https://aimshop.com/products/aim-solo-2-gps-motocross-track-day-lap-timer ·
  https://www.bimmerworld.com/Gauges-Data-Acquisition/Data-Acquisition-Systems/AiM-SOLO2-DL-GPS-Lap-Timer-and-Data-Logger.html ·
  https://sportbiketrackgear.com/aim-solo-2-dl-gps-lap-timer-data-logger ·
  https://www.aimshop.com/products/aim-handlebar-bar-pad-mount-for-solo-2-solo-2-dl ·
  https://product41.com/products/motocross-lap-timer-data-logger
- Starlane Davinci-II: https://hardracing.com/starlane-davinci-ii-r-universal-digital-dash ·
  https://www.sixrace.it/en/cronometri-gps/38553-davinci-ii-r-dashboard-with-integrated-data-acquisition-and-gps.html ·
  https://dynobike.com.au/product/starlane-davinci-ii-r-dashboard-bmw ·
  https://www.moto-vision.com/en/article/74700-dashboard-davinci-ii-rx-series ·
  https://www.gprdirect.com/starlane-davinci-ii-s/
- Starlane Corsaro-II and Stealth GPS-4: https://www.wrs.it/en/instruments/347149-starlane-gps-laptimer-corsaro-ii-r-touch-screen.html ·
  https://www.wrs.it/en/instruments/347192-starlane-gps-laptimer-corsaro-ii-pro-touch-screen.html ·
  https://hhrperformance.com/products/starlane-corsaro-ii-pro ·
  https://dynobike.com.au/product/starlane-stealth-gps-4-tachometer-dashboard-various/ ·
  https://www.motodracing.com/starlane-stealth-gps-4-lite-race-lap-timer
- I2M: https://hhrperformance.com/products/i2m-chrome-pro-2-dash-5-tft-data-logger ·
  https://www.jmraceradiators.com/en/product/i2m-chrome-pro-2-universal/ ·
  https://www.racing-products.com/us/i2m-chrome-pro-2-dashboard.html ·
  https://www.wrs.it/en/instruments/338910-i2m-datalog-pro-universal-data-acquiser.html ·
  https://www.hsbkracing.com/garage-paddock/i2m-electronics/dash-logger/i2m-datalog-pro-data-logger
- MoTeC C125: https://www.motec.com.au/c1series/ ·
  https://www.motec.com.au/hessian/uploads/C125_User_Manual_1f4a1c6bf4.pdf ·
  https://katechengines.com/file/3-C125+Color+Display+Data+Sheet.pdf ·
  https://xtramotorsport.com/product/motec-c125-color-display-non-logging/
- PZRacing: https://www.bike-design.com/en-gb/product/218799/start-next-gps-digital-laptimer ·
  https://www.bike-design.com/en-gb/product/192044/start-basic-gps-digital-laptimer ·
  https://bike-design.com/en-gb/product/198606/start-plus-gps-dashboard-with-data-acquisition ·
  https://www.superbikeunlimited.com/pz-racing-start-next-and-start-basic-gps-lap-timer ·
  https://www.motoblouz.com/en/sale-chronometer-pzracing-gps-start-next-st400-pz-racing-with-wifi-218222.html ·
  https://superbikeunlimited.com/pz-racing-gps-receiver-for-for-oem-dash-board-bmw-s1000rr-m1000rr-2019-2023
- Alfano: https://www.bikesmith.de/en/p/alfano-6-1t-laptimer-display-for-minigp-racing-data-logger-with-gps ·
  https://www.maxiscoot.com/en/product/2t-kit-speedometer-rpm-laptimer-alfano-6-multi-function-86214 ·
  https://www.kartingshop.nl/en/laptimers/6670-alfano-6-laptimer.html ·
  https://www.gopowersports.com/alfano-7-1t-gps-lap-timer-data-logger/
- GET MD60 LOG: https://vitalmx.com/product/guide/Tools,51/GET/MD60-LOG,8953 ·
  https://www.motosport.com/get-md60-log-gps-lap-timer-kit ·
  https://www.revzilla.com/motorcycle/get-athena-md60-gps-lap-timer ·
  https://www.lerepairedesmotards.com/actualites/2010/actu_101203-boitier-chronometre-gps-get-md60.php
- Qstarz, Freelap: https://www.motodracing.com/qstarz-lt-9000s-motorcycle-lap-timer ·
  https://www.freelapusa.com/?p=2836

Loggers without a display

- RaceBox: https://www.racebox.pro/products/racebox-mini/tech-specs ·
  https://racebox.pro/products/racebox-micro/tech-specs ·
  https://www.racebox.pro/info/faq ·
  https://hackster.io/news/the-racebox-micro-is-a-teeny-tiny-25hz-bluetooth-gnss-data-logger-for-rc-racing-and-more-cf7c12fe8929 ·
  https://wrekd.com/products/racebox-micro-25hz-gps-diy-kit-standalone-vehicle-performance-tracker-high-precision-accelerometer-gyroscope
- LITPro: https://apps.apple.com/us/app/litpro-mx-gps-lap-timer/id940870782 ·
  https://ultimatemotorcycling.com/2017/02/13/litpro-review-motocross-tested/ ·
  https://dirtbikes.com/?p=146370 ·
  https://www.vitalmx.com/comment/3986554 ·
  https://www.vitalmx.com/comment/4549966
- RaceCapture: https://www.autosportlabs.com/wp-content/uploads/2018/08/RaceCapture_Pro-MK3-product-brief.pdf ·
  https://wiki.autosportlabs.com/RaceCapture-Pro_MK3 ·
  https://wiki.autosportlabs.com/RaceCapture-Track_MK3 ·
  https://forum.autosportlabs.com/viewtopic.php?p=24824 ·
  https://www.penguingarage.com/products/autosport-labs-racecapture-track-mk3-haltech-can
- Racelogic: https://vboxmotorsport.co.uk/en/vbox-sport ·
  https://vboxmotorsport.co.uk/index.php/en/vbox-touch-motorsport ·
  https://www.gprdirect.com/racelogic-performance-box-touch/ ·
  https://www.edoperformance.com/collections/vendors/products/racelogic-performance-box-touch

Off-road computers

- Voyager Pro / Pro4: https://www.revzilla.com/dirt-bike/trail-tech-voyager-pro-gps-kit ·
  https://www.rockymountainatvmc.com/parts/trail-tech-voyager-pro-gps-computer-p ·
  https://www.utvsource.com/trail-tech-universal-gps-voyager-pro-kit ·
  https://slavensracing.com/shop/voyager-pro-offroad-gps-kit-trail-tech/ ·
  https://upshiftonline.com/products/trail-tech-voyager-pro ·
  https://www.advpulse.com/?p=126740
- Vapor / Endurance II / Striker: https://www.speedaddicts.com/motorcycle/trail-tech-endurance-ii-kit-202-112 ·
  https://www.xpertmotogear.com.au/products/tt202119 ·
  https://sharkleathers.com.au/products/tt752117 ·
  https://www.motosport.com/dirtbike/trail-tech-computers

Street gauges, shift lights

- Koso RX-4: https://www.revzilla.com/motorcycle/koso-rx-4-universal-bluetooth-tft-meter ·
  https://www.jpcycles.com/assets/0007/9142/wh081ba12a-RX4-TFT_上Suzuki-Hayabusa-_KOSO_CORRECTED_compressed-1.pdf ·
  https://www.partsgiant.com/p408460-koso-rx-4-universal-multifunction-meter ·
  https://www.fc-moto.de/en/KOSO-Digital-multifunction-cockpit-RX-4-for-MT-09-17-20
- Motoscope Pro: https://www.jpcycles.com/product/motogadget-motoscope-pro-2-plug-ride-instruments-f ·
  https://shop.bikeexif.com/products/motogadget-motoscope-pro-motorcycle-speedometer ·
  https://cognitomoto.com/products/motoscope-pro ·
  https://louis-moto.co.uk/en/motogadget-motoscope-pro-dashboard-multifunctional-black-10034906 ·
  https://roguemotorcycles.com.au/collections/vendors/products/motogadget-motoscope-pro-dashboard ·
  https://cafe4racer.eu/en/blog/motoscope-pro-speedometer-features-and-setup-b25.html
- Acewell: https://manualshive.com/manual/acewell/ace-6-series/3046619 ·
  https://cafe4racer.eu/en/universal-digital-speedometers-for-motorcycle-and-bikes/1158-7109-acewell-ace-6x54-dashboard.html ·
  https://www.s3performance.com.au/acewell-digital-sports-track-bike-speedometer-with/ACE-6654
- Daytona Velona: https://www.fc-moto.de/en/DAYTONA-VELONA-W-digital-speedometer-and-rev-counter ·
  https://www.louis-moto.com/en/daytona-velona-w-speedometer-tachometer-10015045 ·
  https://roguemotorcycles.com.au/products/daytona-velona-60mm-speedometer-140km-h
- Dakota Digital: https://jj-motorsports.com/dakota-digital-handle-bar-mount-speedometer-with-signals-spiked-black-mlx-5200-k/ ·
  https://teamdreamrides.com/products/dakota-digital-mlx-9214-k-spiked-black-speedometer-with-customizable-display-and-bluetooth-connectivity ·
  https://www.revzilla.com/motorcycle/dakota-digital-big-dog-speedometer
- HealTech: https://www.motospares.com.au/healtech-gi-pro-ds-series-g2-gear-indicator-withou~7324 ·
  https://superbikesupply.com.au/products/healtech-shift-light-pro ·
  https://2wheelhq.com/collections/healtech/products/healtech-shift-light-pro-programmable-shift-light-module ·
  https://www.bikenbiker.com/products/shiftlight-pro-programmable-shift-light-module-healtech
- SureShift, Koso, Daytona indicators: https://www.louis-moto.com/en/motogadget-gear-shift-indicator-with-shift-light-10034672 ·
  https://www.louis-moto.co.uk/en/koso-koso-gear-indicator-with-shift-light-black-10034734 ·
  https://www.partsdenmark.com/products/daytona-lcd-gear-position-indicator

Rally and navigation

- ICO Rallye MAX-G: https://cdn.shopify.com/s/files/1/0369/4061/files/ICO_Rallye_MAX-G_manual_FINAL_EN_PRINT.pdf ·
  https://viejospistones.com/en/product/ico-rally-max-g/ ·
  https://www.rallyraidproducts.co.uk/products/rrp-387-ico-racing-rallye-max-g ·
  https://ixtem-moto.com/en/tripmaster-ico-rally-max-g_p193046.html
- RNS, F2R: https://tripleclampmoto.ca/products/rns-tripmaster-gfx-v2-pro-1 ·
  https://cdn.shopify.com/s/files/1/0061/7252/files/gfx_v2_pro_user_manual_v2.2_english.pdf ·
  https://rebelxsports.com/?p=4480 ·
  https://ixtem-moto.com/en/f2r-amateur-rb850-rallye-navigation-kit.html ·
  https://www.radegarage.com/parts/f2r-race-addon/
- DMD-T865X: https://www.dmdnavigation.com/T865X/ ·
  https://docs.dmdnavigation.com/devicemanuals/T865/ ·
  https://motorcyclenews.com/news/2024/september/dmd-motorcycle-navigation-tablet ·
  https://www.bartang.eu/en/t865x-nav-tab.html ·
  https://aurora-rally.net/product/1084-thork-racing-dmd-t865x-nav-tab-8 ·
  https://accessoryinternational.com/product/thork-racing-dmd-t865x-gps-sat-nav-android-tablet.html
- Carpe Iter: https://rebelxsports.com/product/carpe-iter-adventure-tablet/ ·
  https://www.radegarage.com/?p=51432 ·
  https://shop.carpe-iter.eu/?p=10405 ·
  https://roadrunner.travel/products/carpe-iter-hardware/
- Garmin: https://blog.touratech-usa.com/2026/07/14/how-to-choose-the-right-garmin-motorcycle-navigation-unit/ ·
  https://upshiftonline.com/blogs/content/tech-garmin-gps-comparison ·
  https://ultimatemotorcycling.com/2023/08/14/garmin-zumo-xt2-review-motorcycle-gps-navigator/ ·
  https://shop.rottweilerperformance.com/tread-2 ·
  https://www.gpscentral.ca/garmin-offroad-gps-comparison/
- Beeline Moto II: https://www.brisbanemotorcycles.com.au/product/beeline-moto-ii-metal ·
  https://www.highside.com.au/products/blbld3blk ·
  https://louis.be/nl/louis-edition-10077701
- Chigee AIO-6: https://www.chigee.com/pages/aio-6 ·
  https://chigee.com/pages/know-aio6 ·
  https://www.1000ps.com/en-us/article/3012616/chigee-aio-6-motoros-navigaci-s-kijelz-bemutat-es-teszt ·
  https://prnewswire.co.uk/news-releases/chigee-launches-aio-6-on-kickstarter-customizable-6-inch-smart-display-with-4g-and-insta360-integration-302422024.html ·
  https://manuals.plus/chigee/aio-6-max-smart-riding-system-manual.pdf

OEM

- BMW: https://support.bmw-motorrad.com/s/article/M-1000-R-2023-M-GPS-Laptrigger-data-logger-Ptlfj ·
  https://cyclenews.com/2020/09/article/2021-bmw-m-1000-rr-first-look ·
  https://www.motorauthority.com/news/1129707_bmw-m-1000-rr-is-the-m-division-s-first-motorcycle ·
  https://www.press.bmwgroup.com/india/article/attachment/T0328530EN/475075 ·
  https://www.1000ps.de/businessnews-id-3000233-bmw-datalogger-fuer-s-1000-rr
- Ducati: https://www.ducati.com/us/en/bikes/panigale/panigale-v4/electronics ·
  https://www.ducati.com/nl/nl/bikes/panigale/panigale-v4/electronics ·
  https://www.totalmotorcycle.com/motorcycles/2025/2025-ducati-panigale-v4s
- Yamaha: https://apps.apple.com/us/app/y-trac-data-logger/id1002300813 ·
  https://www.roadracingworld.com/?p=20071 ·
  https://yamahamotorsports.com/models/24-yzf-r1m/features ·
  https://www.totalmotorcycle.com/motorcycles/2016/2016-Yamaha-YZFR1M
- KTM: https://press.ktm.com/news-ktm-takes-its-on-bike-hmi-experience-to-another-level-with-new-tft-displays?id=206497 ·
  https://www.cyclenews.com/2024/10/article/ktm-reveals-new-tft-display-switchgear-and-connectivity-unit ·
  https://transmoto.com.au/explainer-ktms-new-8-tft-displays ·
  https://www.ktm.com/en-gr/tech-guide/tft.html

DIY

- https://github.com/jctoledo/blackbox
- https://github.com/sophienyaa/Honda-Motorcycle-Dash
- https://jpralves.net/post/2020/09/24/honda-motorcycle-dash.html
- https://github.com/jgauchia/icenav-v3
- https://github.com/lspr98/bike-computer-32
- https://relatedrepos.com/gh/remmie1972/Zero-Dashboard
- https://awesome.ecosyste.ms/projects/github.com%2Famrikarisma%2Fmazduino-dash-canbus
- https://relatedrepos.com/gh/MWP/megasquirt-dash-display
- https://github.com/WRansohoff/MotoMate
