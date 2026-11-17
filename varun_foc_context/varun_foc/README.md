# BLDC Motor Controller — Hardware Reference (Board v7)

A 60 mm round, two-layer brushless-DC motor controller built around the **TI DRV8316** three-phase
integrated-FET driver and an **STM32G431CBT6** Cortex-M4F microcontroller. The board accepts a
12–24 V motor supply or USB-C power, generates its own 5 V and 3.3 V rails, and exposes a full
six-PWM interface, three analogue current-sense channels, a fault line, SWD, UART and a spare
sensor input.

This document is a complete engineering description of the board: every component, why it exists
in the schematic, what it connects to, and how the physical layout supports it. It is written to
serve both as a design reference and as the hardware chapter of a project report.

---

## 1. What this board is

| Item | Value |
|---|---|
| Function | Three-phase BLDC/PMSM motor controller (inverter + MCU + power supplies) |
| Motor supply (`VIN`) | 12–24 V nominal, 4.5–35 V at the driver (`VM`), 40 V absolute max |
| Peak motor current | 8 A peak (DRV8316 rating); see §6.6 for the practical continuous limit of the copper |
| Control interface | 6× PWM (`INHx`/`INLx`), 3× current-sense outputs, `nFAULT`, `DRVOFF`, `nSLEEP` |
| Control algorithm | Sensorless FOC (custom firmware); an attempt at sensored FOC using an AS5600 magnetic encoder |
| MCU | STM32G431CBT6, Cortex-M4F + FPU, 170 MHz, 128 KB flash, 32 KB SRAM, LQFP-48 |
| Logic level | 3.3 V |
| Power inputs | Screw terminal (VIN) and USB-C (5 V) with automatic priority muxing |
| Programming | SWD header (primary); USB DFU possible with a BOOT0 pull-up |
| Board size | Ø 60 mm circle with a 2.4 mm flat on one edge; 1.6 mm, 2 layers |
| Mounting | 4× M2 holes on a 50 × 23 mm rectangle |
| Connectors | J1 VIN, J2 motor, J3 USB-C, J5 SWD, J6 sensor, J7 UART |
| Silkscreen title | `BLDC MOTOR CONTROLLER` |

**Team** (as printed on the silkscreen): Varun Nigudkar (231030067), Sarthi Rajorekar (231030059),
Harsh Sagare (231030056).

### Version note (v6 → v7)

The design files in this folder are named `bldc_control_final_v6.*` but live in the `v7` folder, and
the fabrication archive is `fyp_bldc_v7_grbr.zip`. **v7 is a file/version renaming of v6 — the
schematic and layout are electrically and geometrically identical.** Where this document says "v7"
it means "this board"; the file names are historical.

---

## 2. How to read this document

* **Reference designators** (`U4`, `R7`, `C26` …) follow the schematic and the BOM. On the finished
  PCB the silkscreen prints the *part name* for the large ICs (`DRV8316`, `STM32G431x`, `TPS2116`,
  `TPS7A45`, `LMR3651`) instead of `U4`, `U2`, `U8`, `U1`, `U7`. Both forms are used below; the
  netlist appendix (§15) uses the PCB values.
* **Net names** appear in KiCad form with a leading slash (`/VIN_Prot`). In prose the slash is
  dropped (`VIN_Prot`).
* **Pin numbers** are package pin numbers, not pad numbers — they are the same thing here.
* Everything in this document was extracted from the actual KiCad database (`kicad_sch`,
  `kicad_pcb`), the BOM CSV, the DRC reports and the manufacturer datasheets. Values that the BOM
  does not record (zener/TVS voltage, LED colour) are flagged rather than invented.

---

## 3. System architecture

### 3.1 Block diagram

```
POWER
  J1 ──► F1 ──► Q1 ──► VIN_Prot ─┬──► U4 DRV8316 ──► J2  (ph_A/B/C)
  12–24 V  PTC   P-FET           │
                                 └──► U7 LMR36510 ──► 5V_b ─┐
  J3 ──► D5 ──► D6 ──► 5V_p ────────────────────────────────┴──► U8 TPS2116 ──► 5V_syst ──► Q2 ──► U1 ──► 3V3
  USB-C   ESD   Schottky                (priority mux)                              (LDO)

CONTROL
  U2 STM32G431 ── TIM1 CH1/2/3 + CH1N/2N/3N ──► U4 INHx / INLx
  U2 ◄── ADC1_IN1/2/3 (PA0/PA1/PA2) ─────────── U4 SOA / SOB / SOC
  U2 ◄── PA6 ──────────────────────────────── U4 nFAULT  (R6 5.1 k pull-up)
  U2 ──► PA7 ──────────────────────────────── U4 DRVOFF
  U2 ──► PC13 ─────────────────────────────── U4 nSLEEP
  U2 ◄─► PA15 (TIM2_CH1) ──────────────────── J6 sensor
  U2 ◄─► USART1 TX/RX (PB6/PB7) ───────────── J7 ──► ESP32 (wireless link)
  U2 ◄─► SWDIO/SWCLK (PA13/PA14) ──────────── J5 SWD
  U2 ◄─► USB D+/D− (PA11/PA12) ────────────── J3 USB-C
```

### 3.2 Power flow (one line)

```
J1 → F1 (PTC) → Q1 (reverse-polarity P-FET) → VIN_Prot
      └─ D4 zener + R15 gate clamp,  D7 TVS + C25/C2/C9/C23 bulk

VIN_Prot → U7 LMR36510 buck (L1 22 µH) → 5V_b   (5.02 V, 1 A capability)

J3 VBUS → D5 ESD array → D6 SS14 → 5V_p         (USB 5 V, ≈4.7 V after the diode)

5V_b (VIN1) ┐
            ├─ U8 TPS2116 priority mux → 5V_syst
5V_p (VIN2) ┘

5V_syst → Q2 (P-FET load switch, always on) → U1 TPS7A4533 → 3V3
```

### 3.3 Signal flow

```
STM32 TIM1 CH1/CH2/CH3 + CH1N/CH2N/CH3N
        └─ INHC/INHB/INHA + INLC/INLB/INLA ──► U4 (6× PWM mode)
U4 SOA/SOB/SOC ──► PA0/PA1/PA2 (ADC1_IN1..3)
U4 nFAULT ──► PA6 (pull-up R6 5.1 k to 3V3)
U4 DRVOFF ◄── PA7
U4 nSLEEP ◄── PC13
U4 OUTA/B/C ──► J2 pins 3/2/1 (A/B/C)
J6 OUT ──► PA15 (TIM2_CH1, input capture)
J7 ◄─► USART1 TX/RX (PB6/PB7) ──► ESP32 link for wireless control
J5 ◄─► SWDIO/SWCLK (PA13/PA14)
J3 D+/D- ◄─► PA12/PA11 (USB)
```

### 3.4 Design targets and limits

| Parameter | Value | Source |
|---|---|---|
| Driver supply range | 4.5–35 V (40 V abs max) | DRV8316 datasheet |
| Driver peak output | 8 A per phase | DRV8316 datasheet |
| Driver R<sub>DS(on)</sub> | 95 mΩ (HS + LS) at 25 °C | DRV8316 datasheet |
| Max PWM frequency | 200 kHz | DRV8316 datasheet |
| Buck output | 5.02 V, 1 A | LMR36510 + divider |
| LDO output | 3.3 V, 1.5 A (300 mV dropout) | TPS7A4533 |
| Mux current | 2.5 A max | TPS2116 |
| PTC hold current | 5 A at 23 °C (3.33 A at 70 °C) | MF-RHT500 |
| Board copper | 2 layers, 1.6 mm, 35 µm (1 oz) assumed | fabrication data |

---

## 4. Repository contents

| File | What it is |
|---|---|
| `bldc_control_final_v6.kicad_sch` | Single-sheet A3 schematic (KiCad 10, format 20260206) |
| `bldc_control_final_v6.kicad_pcb` | Two-layer PCB, 71 footprints, 524 track segments, 300 vias |
| `bldc_control_final_v6.kicad_pro` | Project settings; netclass `Default` = 0.2 mm track / 0.2 mm clearance, via 0.6/0.3 |
| `bldc_control_final_v6.kicad_dru` | Custom DRC rules file — empty (all checks are KiCad defaults) |
| `bldc_control_final_v6.csv` | BOM, 48 line items, 71 fitted parts |
| `bldc_control_final_v6-*.gbr`, `*.drl` | Gerbers and drill files for fabrication |
| `fyp_bldc_v7_grbr.zip` | Fabrication archive (files inside still carry the `v6` name) |
| `output/drc_report.json` | First full DRC run, 191 findings |
| `output/drc_after.json` | DRC after silkscreen clean-up, 100 findings |
| `bldc_control_final_v6-backups/` | KiCad automatic backups |

> The firmware is **not** part of this repository; it is maintained separately (no public
> repository). §12 describes the hardware/firmware contract the firmware must implement.

---

## 5. Power subsystem

The board has three separate power domains and one mux that joins two of them:

| Rail | Where it comes from | Who uses it |
|---|---|---|
| `VIN` / `VIN_Prot` | J1 through PTC and reverse-polarity FET | DRV8316 `VM`, buck input |
| `5V_b` | LMR36510 buck from `VIN_Prot` | TPS2116 VIN1, PR1 divider, MODE strap |
| `5V_p` | USB-C VBUS through SS14 | TPS2116 VIN2 |
| `5V_syst` | TPS2116 output | LDO input through Q2 |
| `3V3` | TPS7A4533 | MCU, driver logic, pull-ups, headers |
| `PGND` | single power-ground net | everything (poured on both layers) |

### 5.1 Input protection

**Purpose:** accept a 12–24 V motor supply, survive a reversed connection, clamp inductive
transients from the motor cable, and limit fault current without a fuse that has to be replaced.

| Ref | Value | Package | Connection | Role |
|---|---|---|---|---|
| J1 | Phoenix PT-1.5-2-5.0-H, 1×02, 5.00 mm | THT | 1 = `VIN`, 2 = `PGND` | Motor supply entry; silkscreen "VIN +" |
| F1 | Bourns MF-RHT500 PTC | radial THT | `VIN` → `VIN_Fuse` | Resettable over-current protection, 5 A hold / 9 A trip |
| Q1 | BUK6Y33-60PX, P-channel | LFPAK56 | S = `VIN_Fuse`, D = `VIN_Prot`, G = `D4-A` | Reverse-polarity protection; body diode conducts in the correct direction, channel enhances when the gate is pulled low |
| R15 | 100 kΩ | 0805 | gate → `PGND` | Holds the gate low so Q1 is on in normal operation; also discharges the gate when power is removed |
| D4 | Zener (`D_Zener`, value not in BOM) | SOD-123 | cathode = `VIN_Fuse`, anode = gate | Clamps V<sub>GS</sub> to the zener voltage so a 24 V input cannot exceed the FET's ±20 V gate rating |
| D7 | TVS (`D_TVS`, value not in BOM) | SMB | `VIN_Prot` → `PGND` | Clamps supply transients; the stand-off voltage must sit above the maximum input |
| C25 | 0.1 µF | 0805 | `VIN_Prot` → `PGND` | High-frequency decoupling at the driver |
| C2 | 220 nF | 0805 | `VIN_Prot` → `PGND` | Mid-frequency decoupling |
| C9 | 2.2 µF | 0805 | `VIN_Prot` → `PGND` | Local bulk for the buck input |
| C23 | 10 µF | 1210 | `VIN_Prot` → `PGND` | Bulk reservoir next to the DRV8316 `VM` pins |

**How it works.** Current enters at J1, passes through the PTC, then through Q1. Q1 is a
P-channel MOSFET wired "backwards" (source to the input, drain to the load). With the battery
connected correctly, the gate is pulled to ground through R15, V<sub>GS</sub> ≈ −V<sub>IN</sub>,
the channel turns on and the drop is only I·R<sub>DS(on)</sub>. If the supply is reversed, the body
diode blocks and the gate-source voltage is positive, so the FET stays off. D4 protects the gate
oxide by limiting V<sub>GS</sub>; R15 both turns the FET on and defines the current through D4.

**Design numbers.** At 24 V and 3 A the FET dissipates roughly 0.3 W (33 mΩ class device), and the
LFPAK56 drain pad is the only heat path — which is why Q1 sits on a large `VIN_Prot` copper area
(see §10.3). The PTC's own resistance (17.5 mΩ initial) adds about 50 mW at 3 A.

**As-built notes.**

* F1 is rated **16 V maximum** (Bourns MF-RHT500: 5 A hold, 9 A trip, 16 V, 100 A max). At the top
  of the 12–24 V input range the part is above its voltage rating, so treat the resettable-fuse
  function as valid for 12 V-class supplies; for sustained 24 V operation a higher-voltage PTC (for
  example the `/32` variant of the same series) would be the correct part. The PTC also derates:
  3.33 A hold at 70 °C ambient.
* The BOM records D4 and D7 only as generic `D_Zener` / `D_TVS`, so the fitted zener and TVS
  voltages are not documented. D4 must be below the Q1 gate-source limit (a 12–15 V zener is the
  usual choice); D7's stand-off must exceed the highest expected `VIN`.

### 5.2 5 V buck converter (U7 LMR36510)

**Purpose:** generate the 5 V rail that powers the logic and the 3.3 V LDO from the 12–24 V input,
with enough headroom for the LDO's dropout.

| Ref | Value | Package | Connection | Role |
|---|---|---|---|---|
| U7 | LMR36510ADDA | HTSOP-8 with exposed pad | see pin table | 4.2–65 V, 1 A synchronous buck, 400 kHz, internally compensated |
| L1 | 22 µH | Bourns SRN6028 | `SW` → `5V_b` | Buck inductor |
| C12 | 100 nF | 0805 | `BOOT` → `SW` | Bootstrap capacitor for the high-side gate drive |
| C10 | 1 µF | 0805 | `VCC` → `PGND` | Internal regulator decoupling |
| R2 | 100 kΩ | 0805 | `FB` → `5V_b` | Feedback divider top resistor |
| R4 | 24.9 kΩ | 0805 | `FB` → `PGND` | Feedback divider bottom resistor |
| C13 | 22 µF | 1206 | `5V_b` → `PGND` | Output bulk |
| C14 | 22 µF | 1206 | `5V_b` → `PGND` | Output bulk |
| C15 | 1 µF | 0805 | `5V_b` → `PGND` | Output high-frequency decoupling |
| TP6 | test point | 1.5 mm pad | `5V_b` | Buck output probe |

**Pin usage (U7).**

| Pin | Name | Net | Note |
|---|---|---|---|
| 1 | PGND | `PGND` | Power ground |
| 2 | VIN | `VIN_Prot` | Input |
| 3 | EN | `VIN_Prot` | Precision enable tied to the input — the buck starts as soon as the input is valid |
| 4 | PG | `PGND` | Power-good is open-drain and unused, so it is tied low |
| 5 | FB | divider | 1.0 V reference |
| 6 | VCC | `C10` | Internal 5 V regulator |
| 7 | BOOT | `C12` | Bootstrap |
| 8 | SW | `L1` | Switch node |
| 9 | PAD | `PGND` | Thermal pad, stitched to the ground pour |

**Output voltage.**

```
Vout = VREF × (1 + R2/R4) = 1.0 V × (1 + 100k/24.9k) = 5.016 V
```

The 1 % resistors give a worst-case window of about 4.93–5.10 V, comfortably inside the TPS2116's
1.6–5.5 V input range and above the 3.6 V the LDO needs for regulation.

**Inductor ripple.**

```
ΔIL = Vout × (1 − D) / (L × fsw),   D = Vout/Vin
Vin = 24 V:  D = 0.209,  ΔIL = 0.45 A p-p
Vin = 12 V:  D = 0.418,  ΔIL = 0.33 A p-p
```

At the board's actual logic load (≈40 mA) the converter runs in PFM/DCM, so the ripple figure
matters only for the peak-current limit and the output-capacitor ripple rating, both of which are
far from being stressed.

**Layout.** U7 sits at the left-centre of the board (172.35, 100.75 mm) with L1 immediately to its
left and C13/C14/C15 clustered on the `5V_b` side. The `SW` node is kept as a short, wide
(1.0 mm) track between pin 8, C12 and L1, and the exposed pad is stitched to the ground pour with
four 0.6/0.3 vias.

### 5.3 USB-C input

**Purpose:** power the logic (and program the MCU) from a USB-C cable when the 24 V supply is not
connected, and provide a USB 2.0 full-speed data path to the STM32.

| Ref | Value | Package | Connection | Role |
|---|---|---|---|---|
| J3 | GCT USB4110, USB-C receptacle, 16 pins | THT/SMD hybrid | VBUS, GND, CC1, CC2, D+, D−, SBU1/2 | Connector; shield tabs to `PGND` |
| D5 | SP0503BA, 4-channel TVS array | SOT-143 | pin 4 `VBUS`, pin 3 `D+`, pin 2 `D−`, pin 1 `PGND` | ESD protection for the three USB lines |
| D6 | SS14 Schottky | SMA | anode `VBUS`, cathode `5V_p` | Blocks reverse current from the board back into the host |
| C16 | 1 µF | 0805 | `5V_p` → `PGND` | USB-side bulk |
| R12 | 5.1 kΩ | 0805 | `CC1` → `3V3` | CC termination |
| R13 | 5.1 kΩ | 0805 | `CC2` → `3V3` | CC termination |
| TP4 | test point | 1.5 mm pad | `5V_p` | USB rail probe |

**Behaviour.** VBUS from the connector passes the TVS array, then D6, and lands on `5V_p`. With a
5 V host the rail sits at about 4.7 V, which is still above the LDO's 3.6 V minimum. D6 also stops
the board's own 5 V from feeding back into a host that has suspended its port.

**As-built note (CC termination).** R12/R13 pull CC1/CC2 **up to 3V3** (5.1 kΩ to the rail). A
USB-C *sink* is defined by 5.1 kΩ to **GND** (Rd); a 5.1 kΩ pull-up is the *source* (Rp) signature.
Because the board presents Rp on both CC pins, a USB-C host may refuse to apply VBUS or may apply it
in the wrong current mode, and a USB-C charger will not recognise the board as a sink. USB power
therefore works reliably only with a host/charger that supplies VBUS regardless of CC (or with an
A-to-C cable, which has no CC negotiation). The D+/D− pair is unaffected, so SWD programming and
USB data still function; if USB power is required from a strict USB-C source, R12/R13 should be
reworked to 5.1 kΩ to GND.

### 5.4 Power mux (U8 TPS2116)

**Purpose:** make the 5 V buck the primary source and automatically fall back to USB 5 V when the
motor supply is absent or collapsing, without glitching the 3.3 V rail.

| Ref | Value | Package | Connection | Role |
|---|---|---|---|---|
| U8 | TPS2116DRL | SOT-583-8 | see pin table | 1.6–5.5 V, 2.5 A priority power mux |
| R10 | 30 kΩ | 0805 | `5V_b` → `PR1` | Divider top |
| R14 | 10 kΩ | 0805 | `PR1` → `PGND` | Divider bottom |
| D3 | LED | 0603 | anode `ST`, cathode `PGND` | Status indicator (see note) |
| TP8 | test point | 1.5 mm pad | `5V_syst` | Mux output probe |

**Pin usage (U8).**

| Pin | Name | Net | Note |
|---|---|---|---|
| 1 | GND | `PGND` | |
| 2, 7 | VOUT | `5V_syst` | Both output pins are used |
| 3 | VIN1 | `5V_b` | Primary (buck) |
| 4 | PR1 | divider | Priority threshold input |
| 5 | MODE | `5V_b` | Tied to VIN1 → **priority mode** |
| 6 | VIN2 | `5V_p` | Secondary (USB) |
| 8 | ST | `D3` anode | Open-drain status |

**Priority threshold.**

```
VPR1 = 5V_b × R14/(R10 + R14) = 5V_b × 10/40 = 0.25 × 5V_b
PR1 crosses VREF (1.0 V) when 5V_b = 4.0 V
```

So the mux stays on the buck while `5V_b` is above 4.0 V (at 5.0 V, PR1 = 1.25 V — 250 mV of
margin) and hands over to USB below that. Because the divider is fed from `5V_b` itself, the
handover is monotonic: as the buck collapses, PR1 falls with it and the switch is clean. The
TPS2116 performs break-before-make switching, and its output capacitance (the LDO input network,
C3 + C11 ≈ 101 µF) carries the 3.3 V rail through the few-microsecond gap.

**As-built note (D3).** D3's anode is wired directly to `ST` and its cathode to `PGND`, with no
series resistor and no pull-up. `ST` is an open-drain output that pulls *low* when VIN1 is not in
use, so with this connection the LED has no defined current path in normal operation and no
current limit. Treat D3 as an unpopulated/non-functional indicator; the rail state is better
observed at TP8.

### 5.5 3.3 V LDO (U1 TPS7A4533)

**Purpose:** produce the clean 3.3 V logic and analogue rail, and provide a manual-ish isolation
point (Q2) between the muxed 5 V and the LDO.

| Ref | Value | Package | Connection | Role |
|---|---|---|---|---|
| Q2 | AO3407A, P-channel | SOT-23 | S = `5V_syst`, D = `U1-IN`, G via R18 to `PGND` | Load switch; gate held low so it is permanently on |
| R18 | 100 kΩ | 0805 | gate → `PGND` | Gate pull-down |
| U1 | TPS7A4533 | SOT-223-6 | IN = `U1-IN`, OUT = `3V3` | 3.3 V fixed, 1.5 A, 300 mV dropout, low noise |
| C3 | 100 µF | 0805 | `U1-IN` → `PGND` | Input bulk (low-voltage ceramic in an 0805 body) |
| C11 | 1 µF | 0805 | `U1-IN` → `PGND` | Input high-frequency decoupling |
| C8 | 10 µF | 0805 | `3V3` → `PGND` | Output bulk (the datasheet's minimum for stability) |
| C28 | 0.1 µF | 0805 | `3V3` → `PGND` | Output high-frequency decoupling |
| C18 | 0.1 µF | 0805 | `3V3` → `PGND` | MCU local decoupling |
| C21 | 0.1 µF | 0805 | `3V3` → `PGND` | MCU local decoupling |
| C19 | 1 µF | 0805 | `3V3` → `PGND` | MCU local decoupling |
| C20 | 1 µF | 0805 | `3V3` → `PGND` | MCU local decoupling |
| R3 | 1 kΩ | 0805 | `3V3` → `D1` anode | Power LED series resistor |
| D1 | LED | 0603 | cathode `PGND` | 3.3 V present indicator |
| TP1 | test point | 1.5 mm pad | `3V3` | Logic rail probe |

**Pin usage (U1).**

| Pin | Name | Net | Note |
|---|---|---|---|
| 1 | ~SHDN | `U1-IN` | Tied to the input → permanently enabled |
| 2 | IN | `U1-IN` | Input |
| 3 / tab | GND | `PGND` | Ground and heat spreader |
| 4 | OUT | `3V3` | Regulated output |
| 5 | SENSE | `3V3` | Kelvin sense tied to the output |

**Numbers.** Dropout 300 mV at 1.5 A, so 5 V in is ample. At the board's ~35 mA logic load the
dissipation is only (5.0 − 3.3) × 0.035 ≈ 60 mW. Output noise is 35 µV<sub>RMS</sub> (10 Hz–100 kHz)
and ripple rejection 68 dB at 1 kHz, which is why this part was chosen over a cheap 1117-class LDO:
the same rail feeds the ADC reference and the current-sense amplifier reference.

**Q2's role.** The P-FET is not switched by the MCU; its gate is tied to ground through R18, so it
is always on. It exists as a physical break between the mux output and the LDO input — useful for
current measurement, fault isolation or a future soft-start — and it adds only a few tens of
millivolts of drop at the load currents involved.

### 5.6 Power budget

| Load | Rail | Current (typ) | Notes |
|---|---|---|---|
| STM32G431 at 170 MHz, peripherals on | 3V3 | 25–30 mA | HSI-based clock, no crystal |
| DRV8316 `VREF` input | 3V3 | 50 µA | Reference pin current |
| D1 power LED | 3V3 | ≈1.3 mA | (3.3 − 2.0)/1 kΩ |
| `nFAULT` pull-up, CC resistors | 3V3 | <1 mA | Only when active / cable attached |
| **3V3 total** | | **≈35 mA** | |
| LDO quiescent | 5V_syst | 1 mA | |
| **5 V total** | | **≈40 mA** | Buck is sized for 1 A — 25× margin |
| DRV8316 quiescent (awake, no motor) | VIN_Prot | ≈5–10 mA | Plus gate-drive losses when switching |
| Motor | VIN_Prot | up to 8 A peak | Dominated by the load, not the board |

The logic side therefore draws well under 0.25 W. The dimensioning driver is the motor current and
the copper, not the regulators.

### 5.7 Power-up sequence

1. `VIN` appears → F1, Q1 conduct → `VIN_Prot` rises (D7 clamps any overshoot).
2. U7's EN is tied to `VIN_Prot`; once the input passes UVLO the buck starts and `5V_b` ramps.
3. `PR1` rises with `5V_b`; above 4.0 V the TPS2116 selects VIN1 and `5V_syst` follows.
4. Q2 is already on; U1 regulates `3V3`.
5. The MCU leaves reset, drives `nSLEEP` (PC13) high, keeps `DRVOFF` (PA7) high until the firmware
   is ready, then releases the driver.
6. If USB is plugged in while `VIN` is absent, steps 3–5 happen with `5V_p` as the source; the
   handover when `VIN` returns is automatic and glitch-free.

---

## 6. Motor drive stage (U4 DRV8316)

### 6.1 Overview

U4 is a three-phase integrated-FET driver: three half-bridges with 95 mΩ (HS+LS) N-channel FETs,
gate drivers with a charge pump for the high side, three low-side current-sense amplifiers, and a
full protection set (UVLO, charge-pump UVLO, over-current, thermal warning/shutdown, `nFAULT`). It
needs no external shunt resistors, which is what makes a 60 mm two-layer board practical.

| Attribute | Value |
|---|---|
| Part fitted | **DRV8316T** (hardware-configured variant) |
| Part named in BOM/symbol | `DRV8316RRGFR` (SPI variant) |
| Package | VQFN-40, 7 × 5 mm, exposed thermal pad (pad 41) |
| Supply | `VM` = `VIN_Prot`, pins 9/10/11 |
| Outputs | `OUTA` 13/14, `OUTB` 16/17, `OUTC` 19/20 |
| Interface | 6× PWM (`INHA/INLA` …), 3× current sense, `nFAULT`, `DRVOFF`, `nSLEEP` |

**Variant note.** The schematic symbol and BOM say `DRV8316RRGFR` (the SPI part), but the board is
wired for the hardware variant: pin 33 goes to a strap resistor (`MODE`), pin 34 to a strap
resistor (`SLEW`), pins 35/36 are left open (`OCP/SR`, `GAIN`) and pin 24 is grounded
(`VSEL_BK`). On the SPI part those pins are `SDO`, `SDI`, `SCLK`, `nSCS` and `NC`. The two parts are
pin-compatible, and the **DRV8316T is the device actually fitted**. Consequence: there is no SPI bus
to the driver, all configuration is by hardware straps, and fault diagnosis is limited to the
`nFAULT` pin.

### 6.2 Pin map (DRV8316T, as wired)

| Pin | Name (T variant) | Net | Function in this design |
|---|---|---|---|
| 1 | NC | — | Not connected |
| 2 | AGND | `PGND` | Analogue ground |
| 3 | FB_BK | `FB_BK` | Buck feedback (R5/C29 network) |
| 4 | GND_BK | `PGND` | Buck ground |
| 5 | SW_BK | `SW_BK` | Buck switch node → R5 |
| 6 | CPL | `CPL` | Charge-pump flying capacitor (C27) |
| 7 | CPH | `CPH` | Charge-pump flying capacitor (C27) |
| 8 | CP | `CP` | Charge-pump reservoir, C26 1 µF to `VIN_Prot` |
| 9, 10, 11 | VM | `VIN_Prot` | Motor supply |
| 12 | PGND | `PGND` | Power ground |
| 13, 14 | OUTA | `ph_A` | Phase A → J2 pin 3 |
| 15 | PGND | `PGND` | Power ground |
| 16, 17 | OUTB | `ph_B` | Phase B → J2 pin 2 |
| 18 | PGND | `PGND` | Power ground |
| 19, 20 | OUTC | `ph_C` | Phase C → J2 pin 1 |
| 21 | DRVOFF | `DRVOFF` | High = all six FETs off (PA7) |
| 22 | nFAULT | `FAULT_N` | Open-drain fault, R6 5.1 k to 3V3 (PA6) |
| 23 | nSLEEP | `SLEEP_N` | Low = sleep; also a 20–40 µs low pulse resets a fault (PC13) |
| 24 | VSEL_BK | `PGND` | Buck output select = 3.3 V (level 1) |
| 25 | AVDD | `AVDD` | Internal 3.3 V regulator output; C24 0.1 µF; R7 pulls `SLEW` to it |
| 26 | AGND | `PGND` | Analogue ground |
| 27 | INHA | `INHA` | Phase A high side ← PA10 / TIM1_CH3 |
| 28 | INLA | `INLA` | Phase A low side ← PB15 / TIM1_CH3N |
| 29 | INHB | `INHB` | Phase B high side ← PA9 / TIM1_CH2 |
| 30 | INLB | `INLB` | Phase B low side ← PB14 / TIM1_CH2N |
| 31 | INHC | `INHC` | Phase C high side ← PA8 / TIM1_CH1 |
| 32 | INLC | `INLC` | Phase C low side ← PB13 / TIM1_CH1N |
| 33 | MODE | `MODE` | R8 0 Ω to `PGND` → 6× PWM mode |
| 34 | SLEW | `SLEW` | R7 47 k to `AVDD` → 125 V/µs |
| 35 | OCP/SR | — | Open (Hi-Z level, mode 3) |
| 36 | GAIN | — | Open (Hi-Z level) → 0.3 V/A |
| 37 | VREF/ILIM | `3V3` | CSA reference tied to the 3.3 V rail |
| 38 | SOC | `SOC` | Phase C current sense → PA2 / ADC1_IN3 |
| 39 | SOB | `SOB` | Phase B current sense → PA1 / ADC1_IN2 |
| 40 | SOA | `SOA` | Phase A current sense → PA0 / ADC1_IN1 |
| 41 | Thermal pad | `PGND` | Must be grounded; carries the heat into the pour |

### 6.3 Hardware configuration straps

Because the T variant is fitted, the four "SPI" pins become resistor-programmed analogue inputs.
Each is a multi-level input sampled at power-up (changes require a power cycle or sleep/wake).

| Setting | Pin | Strap | Selected level | Effect |
|---|---|---|---|---|
| PWM mode | 33 `MODE` | R8 = 0 Ω to `PGND` | Level 1 (AGND) | **6× PWM**: each `INHx`/`INLx` drives its own FET, so the MCU must supply complementary PWM with dead time |
| Slew rate | 34 `SLEW` | R7 = 47 kΩ to `AVDD` | Level 3 | 125 V/µs phase-node slew (dead time ≈ 1 µs, propagation ≈ 1.1 µs) |
| CSA gain | 36 `GAIN` | open | Hi-Z (level 2) | 0.3 V/A |
| OCP / active demag | 35 `OCP/SR` | open | Hi-Z (level 3) | Active demagnetisation enabled; documented OCP trip points are 16 A (AGND) and 24 A (AVDD) |
| Buck output | 24 `VSEL_BK` | tied to `PGND` | Level 1 | 3.3 V (buck itself is not used as a rail) |
| CSA reference | 37 `VREF` | `3V3` | 3.3 V | Outputs referenced to the ADC's own reference — ratiometric sensing |

**Why 6× PWM matters.** In 6× mode the driver does not insert its own dead time between the high
and low inputs of a phase; it passes them through. The STM32's TIM1 complementary outputs with
programmed dead time therefore define the shoot-through margin. The 125 V/µs slew setting is a
reasonable compromise between EMI and switching loss at 24 V, and it keeps the required dead time
near 1 µs.

### 6.4 Charge pump, AVDD and the internal buck

| Ref | Value | Connection | Purpose |
|---|---|---|---|
| C26 | 1 µF | `CP` → `VIN_Prot` | Charge-pump reservoir; must be rated for ≥2× VM |
| C27 | 47 nF | `CPH` ↔ `CPL` | Flying capacitor for the high-side gate supply |
| C24 | 0.1 µF | `AVDD` → `PGND` | Internal 3.3 V regulator decoupling |
| R5 | 22 Ω | `SW_BK` → `FB_BK` | Buck "resistor mode" element |
| C29 | 22 µF | `FB_BK` → `PGND` | Buck output capacitor |

The high-side gates are driven from a charge pump referenced to `VM`; C26 and C27 are its entire
external network. `AVDD` is the driver's internal 3.3 V regulator (30 mA capability) and is used
here only as the reference for the `SLEW` strap — it is **not** connected to the board's `3V3`
rail, so the MCU rail and the driver's analogue rail are deliberately separate.

**The internal buck is present but unused as a system rail.** The DRV8316T cannot disable its buck,
and TI requires the `SW_BK`/`FB_BK` network to be fitted even when the output is not used. R5 (22 Ω)
and C29 (22 µF) are exactly TI's documented "resistor instead of inductor" configuration: the buck
regulates 3.3 V (selected by `VSEL_BK` = AGND) into that RC, with a 40 mA capability, and nothing
else is connected to it. All system 5 V comes from U7 and all 3.3 V from U1. This is why there is
no second inductor on the board.

### 6.5 Current sensing

Each half-bridge has an integrated low-side shunt and amplifier; the amplified result appears on
`SOA`/`SOB`/`SOC` (pins 40/39/38) and goes straight to three ADC pins.

| Parameter | Value |
|---|---|
| Gain (GAIN = Hi-Z) | 0.3 V/A |
| Full-scale measurement | ±8 A |
| Output linear range | 0.25 V … VREF − 0.25 V |
| Reference | `VREF` = 3.3 V (same node as the ADC's `VREF+`) |
| Gain error | ±9.5 % (I < 4 A) |
| Offset (0 A) | ±50 mA equivalent |

Because `VREF` and the MCU's `VREF+` are the same 3.3 V net, the measurement is ratiometric: a
drift in the 3.3 V rail moves the ADC reference and the amplifier reference together, cancelling
to first order. With 0.3 V/A and a 0.25–3.05 V linear window around the mid-scale bias, roughly
±4.6 A per phase can be resolved before the amplifier output clips — enough for a 3 A RMS motor,
while the 8 A peak rating is only reachable in the analogue domain (the ADC saturates first).

No external filter is fitted. The DRV8316 outputs tolerate a capacitive load or an RC low-pass;
if the firmware needs cleaner samples, a series resistor plus capacitor at each `SOx` pin can be
added, but the existing layout gives each sense trace its own short run to the MCU.

### 6.6 Output stage and J2

| Ref | Value | Connection |
|---|---|---|
| J2 | Phoenix PT-1.5-3-5.0-H, 1×03, 5.00 mm | 1 = `ph_C`, 2 = `ph_B`, 3 = `ph_A` |

Each phase uses two package pins in parallel (13+14, 16+17, 19+20) to halve the bond-wire and
lead resistance, and each pair leaves the driver on 1.3 mm tracks. The silkscreen marks the
terminals `A`, `B`, `C`.

**Copper limit.** On 35 µm copper, a 1.3 mm external trace carries roughly 3–3.5 A at a 10 °C rise;
1.0 mm carries about 2.5 A. The 8 A figure is the silicon's peak rating and is only appropriate for
short bursts — the board's continuous phase current should stay in the 2–3 A region unless the
traces are reinforced. The `PGND` return is a full pour on both layers (174 stitching vias), so the
return path is not the limiting element.

**Thermal path.** The exposed pad (pin 41) is soldered to a `PGND` land that is stitched into both
ground pours. The custom footprint also carries a via field at the pad centre; in this database all
of those vias sit at the same coordinate (191.50, 90.25 mm), so the drill file contains 13
coincident hits at that point rather than a spread array. The pad is still connected to both pours
through the surrounding stitching, but the thermal spreading is less even than a distributed array
would give.

---

## 7. Microcontroller (U2 STM32G431CBT6)

### 7.1 Overview

The STM32G431CBT6 is a Cortex-M4F with FPU and DSP instructions at up to 170 MHz, 128 KB of flash
and 32 KB of SRAM in LQFP-48. It was chosen for three reasons: a TIM1 advanced timer with three
complementary pairs and dead-time insertion (exactly what the DRV8316 needs), three fast 12-bit
ADCs with injected-conversion groups that can be triggered by a timer, and a CORDIC/FMAC
accelerator that speeds up Park/Clarke transforms in FOC.

| Attribute | Value |
|---|---|
| Core | ARM Cortex-M4F, 170 MHz, single-precision FPU |
| Memory | 128 KB flash, 32 KB SRAM |
| Package | LQFP-48, 7 × 7 mm, 0.5 mm pitch |
| Supply | 3.3 V on pins 1, 20, 21, 24, 36, 48 |
| Ground | pins 19 (VSSA), 23, 35, 47 |
| ADC reference | `VREF+` (pin 20) tied directly to `3V3` |
| External clock | **none** (no HSE crystal, no LSE) |

### 7.2 Pin map (all 48 pins, as wired)

| Pin | Port | Net | Function here | Notable alternates |
|---|---|---|---|---|
| 1 | VBAT | `3V3` | Backup-domain supply tied to the main rail | — |
| 2 | PC13 | `SLEEP_N` | DRV8316 `nSLEEP` enable | TIM1_CH1N (AF4), TIM1_BKIN (AF2), RTC |
| 3 | PC14 | — | Not connected | OSC32_IN |
| 4 | PC15 | — | Not connected | OSC32_OUT |
| 5 | PF0 | — | Not connected | OSC_IN, ADC1_IN10 |
| 6 | PF1 | — | Not connected | OSC_OUT, ADC2_IN10 |
| 7 | PG10 | `NRST` | Reset; SW4 and C17 (see §7.4) | NRST, DAC |
| 8 | PA0 | `SOA` | Phase A current sense | ADC1_IN1, TIM2_CH1/ETR, COMP1 |
| 9 | PA1 | `SOB` | Phase B current sense | ADC1_IN2, TIM2_CH2, OPAMP1_VINP |
| 10 | PA2 | `SOC` | Phase C current sense | ADC1_IN3, TIM2_CH3, OPAMP1_VOUT |
| 11 | PA3 | `PA3` | Spare, brought to a labelled net | ADC1_IN4, TIM2_CH4, USART2_RX |
| 12 | PA4 | — | Not connected | ADC2_IN17, DAC1_OUT1 |
| 13 | PA5 | `PA5` | Spare, brought to a labelled net | ADC2_IN13, DAC1_OUT2, TIM2_CH1 |
| 14 | PA6 | `FAULT_N` | DRV8316 fault input (R6 pull-up) | **TIM1_BKIN (AF6)**, ADC2_IN3 |
| 15 | PA7 | `DRVOFF` | Driver output-disable control | TIM1_CH1N (AF6), ADC2_IN4 |
| 16 | PB0 | `PB0` | Spare | ADC1_IN15, TIM1_CH2N (AF6), TIM3_CH3 |
| 17 | PB1 | `PB1` | Spare | ADC1_IN12, TIM1_CH3N (AF6) |
| 18 | PB2 | `PB2` | Spare | ADC2_IN12, LPTIM1_OUT |
| 19 | VSSA | `PGND` | Analogue ground | — |
| 20 | VREF+ | `3V3` | ADC reference (ratiometric with the CSA) | VREFBUF_OUT |
| 21 | VDDA | `3V3` | Analogue supply | — |
| 22 | PB10 | `I2C1_SCL` | I2C1 clock, labelled but no header | TIM2_CH3, USART3_TX |
| 23 | VSS | `PGND` | Digital ground | — |
| 24 | VDD | `3V3` | Digital supply | — |
| 25 | PB11 | `I2C1_SDA` | I2C1 data, labelled but no header | ADC1_IN14, TIM2_CH4, USART3_RX |
| 26 | PB12 | `PB12` | Spare | ADC1_IN11, TIM1_BKIN (AF6), SPI2_NSS |
| 27 | PB13 | `INLC` | Phase C low side | **TIM1_CH1N (AF6)** |
| 28 | PB14 | `INLB` | Phase B low side | **TIM1_CH2N (AF6)** |
| 29 | PB15 | `INLA` | Phase A low side | **TIM1_CH3N (AF4)** |
| 30 | PA8 | `INHC` | Phase C high side | **TIM1_CH1 (AF6)**, I2C3_SCL, MCO |
| 31 | PA9 | `INHB` | Phase B high side | **TIM1_CH2 (AF6)**, USART1_TX |
| 32 | PA10 | `INHA` | Phase A high side | **TIM1_CH3 (AF6)**, USART1_RX |
| 33 | PA11 | `D-` | USB D− | USB_DM (AF10), TIM1_CH1N, FDCAN1_RX |
| 34 | PA12 | `D+` | USB D+ | USB_DP (AF10), TIM1_CH2N, FDCAN1_TX |
| 35 | VSS | `PGND` | Digital ground | — |
| 36 | VDD | `3V3` | Digital supply | — |
| 37 | PA13 | `SWDIO` | SWD data | SYS_JTMS-SWDIO (AF0) |
| 38 | PA14 | `SWCLK` | SWD clock | SYS_JTCK-SWCLK (AF0), TIM1_BKIN |
| 39 | PA15 | `OUT` | J6 sensor input | **TIM2_CH1 (AF1)**, TIM2_ETR (AF14), TIM1_BKIN; **no ADC** |
| 40 | PB3 | — | Not connected | SWO (AF0), TIM2_CH2 |
| 41 | PB4 | — | Not connected | TIM3_CH1, UCPD1_CC2 |
| 42 | PB5 | — | Not connected | I2C1_SMBA, TIM3_CH2 |
| 43 | PB6 | `USART1_TX` | UART TX to J7 | USART1_TX (AF7), TIM4_CH1 |
| 44 | PB7 | `USART1_RX` | UART RX from J7 | USART1_RX (AF7), TIM4_CH2 |
| 45 | PB8 | `/PB9` | **BOOT0** (multiplexed), no header, no resistor | BOOT0, TIM1_BKIN, FDCAN1_RX |
| 46 | PB9 | `PB9` | 10 kΩ to ground (R1) | FDCAN1_TX, TIM1_CH3N (AF12), I2C1_SDA |
| 47 | VSS | `PGND` | Digital ground | — |
| 48 | VDD | `3V3` | Digital supply | — |

**PWM-to-phase mapping.** The channel assignment is deliberately "crossed": phase C is TIM1_CH1,
phase B is CH2, phase A is CH3. Firmware must respect this, or the motor will run with a rotated
commutation order:

| Phase | High side | Low side |
|---|---|---|
| A | PA10 = TIM1_CH3 | PB15 = TIM1_CH3N |
| B | PA9 = TIM1_CH2 | PB14 = TIM1_CH2N |
| C | PA8 = TIM1_CH1 | PB13 = TIM1_CH1N |

### 7.3 Clocking

There is **no crystal** on the board: PF0/PF1 (HSE) and PC14/PC15 (LSE) are unconnected. Firmware
must run from the internal oscillators — HSI16 into the PLL for the core clock, and HSI48 with the
CRS (clock recovery system) locked to USB SOF if USB is used. HSI16 is ±1 % over temperature, which
is fine for PWM, UART and FOC, but is not accurate enough on its own for USB full-speed, hence the
need for HSI48/CRS in any USB-enabled build.

### 7.4 Reset and boot

| Ref | Value | Connection | Role |
|---|---|---|---|
| SW4 | Alps SKRK SPST-NO | pin 1 `3V3`, pin 2 `NRST` | Reset button (as drawn, see note) |
| C17 | 0.1 µF | `NRST` ↔ `3V3` | Noise filter on the reset line |
| R1 | 10 kΩ | `PB9` → `PGND` | Pull-down on PB9 |

**As-built note (reset).** NRST is active-low and has an internal pull-up. SW4 connects NRST to
**3V3**, and C17 also returns to 3V3, so pressing the button drives the pin to the same level the
internal pull-up already holds it at — it does not generate a reset pulse. In practice: reset the
part over SWD (the debug probe drives NRST), or momentarily short NRST to `PGND` (TP1's neighbour
pad or the C17/NRST pad) to force a reset. Re-working SW4's pin 1 to `PGND` would make the button
behave as intended.

**Boot.** BOOT0 is multiplexed with PB8 on pin 45. The net on that pad is labelled `/PB9`, the pad
is not broken out to any header, and there is no pull resistor on it — the 10 kΩ R1 sits on PB9
(pin 46) instead. Consequently:

* Normal operation relies on the BOOT0 option bit / internal behaviour; the part boots from flash
  as shipped.
* To enter the ROM DFU bootloader (USB programming), temporarily pull pin 45 high at reset — a
  fine wire or a probe — or set `nBOOT0`/`nBOOT_SEL` option bits over SWD.
* SWD is the primary and recommended programming path.

### 7.5 SWD header (J5)

| Pin | Signal | MCU pin |
|---|---|---|
| 1 | `3V3` | — |
| 2 | `SWDIO` | PA13 (37) |
| 3 | `SWCLK` | PA14 (38) |
| 4 | `PGND` | — |

A standard 4-wire probe (ST-Link, J-Link, DAPLink) connects directly; there is no reset wire, so
"connect under reset" needs the NRST pad. Pin 1 exposes the board's own 3.3 V rail — do not use it
to power the board from the probe while the LDO is also enabled.

### 7.6 USB

| Signal | MCU pin | Connector |
|---|---|---|
| D− | PA11 (33) | J3 A7/B7 |
| D+ | PA12 (34) | J3 A6/B6 |

USB 2.0 full speed only (the 16-pin receptacle has no SuperSpeed pairs). D5 clamps all three lines
to `PGND`. There are no series termination resistors on D+/D−; the STM32's internal drivers and the
short trace length (≈15 mm) make them unnecessary at 12 Mbit/s. See §5.3 for the CC-resistor note.

### 7.7 UART header (J7)

| Pin | Signal | MCU pin | Direction |
|---|---|---|---|
| 1 | `USART1_TX` | PB6 (43) | MCU → ESP32 |
| 2 | `USART1_RX` | PB7 (44) | ESP32 → MCU |

This is the link to the ESP32 module used for wireless motor control: the ESP32 runs the Wi-Fi/BLE
stack and forwards set-points and telemetry, while the STM32 keeps the real-time FOC loop. The
header carries no 5 V or 3.3 V pin, so the ESP32 must be powered separately — and because the STM32
is 3.3 V, any 5 V ESP32 board needs level shifting on its TX line.

### 7.8 Sensor input (J6)

| Pin | Signal | MCU pin | Capability |
|---|---|---|---|
| 1 | `3V3` | — | Sensor supply (≈35 mA available) |
| 2 | `OUT` | PA15 (39) | Digital / timer input — **not** ADC-capable |
| 3 | `PGND` | — | Ground |

J6 is a general single-signal sensor port: a Hall/tachometer output, or the output of an AS5600
magnetic encoder. PA15 is TIM2_CH1, so frequency, duty cycle and pulse width can all be captured in
hardware. It cannot sample an analogue voltage — see §13.

### 7.9 Spare and unused IO

| Pin | Port | State | Best use if needed |
|---|---|---|---|
| 11 | PA3 | labelled net `PA3` | ADC1_IN4 — analogue sensor input |
| 13 | PA5 | labelled net `PA5` | ADC2_IN13 / DAC1_OUT2 — debug DAC |
| 16 | PB0 | labelled net `PB0` | ADC1_IN15, TIM1_CH2N |
| 17 | PB1 | labelled net `PB1` | ADC1_IN12, TIM1_CH3N |
| 18 | PB2 | labelled net `PB2` | ADC2_IN12 |
| 26 | PB12 | labelled net `PB12` | ADC1_IN11, SPI2_NSS |
| 22 | PB10 | `I2C1_SCL` | I2C1 clock (no header) |
| 25 | PB11 | `I2C1_SDA` | I2C1 data (no header) |
| 46 | PB9 | 10 k to GND | GPIO (pulldown fitted) |
| 3, 4, 5, 6 | PC14, PC15, PF0, PF1 | unconnected | Oscillator pins only |
| 12, 40, 41, 42 | PA4, PB3, PB4, PB5 | unconnected | — |

The "labelled net" pins have a named net in the database but no pad, via or connector — they are
test-point-style stubs intended for a future revision or a flying lead.

---

## 8. Connectors, test points and indicators

### 8.1 Connector summary

| Ref | Type | Pins | Signals | Mechanical |
|---|---|---|---|---|
| J1 | Phoenix PT-1.5-2-5.0-H | 2 | `VIN`, `PGND` | 5.00 mm pitch, right-angle, bottom edge |
| J2 | Phoenix PT-1.5-3-5.0-H | 3 | `ph_C`, `ph_B`, `ph_A` | 5.00 mm pitch, right-angle, right edge |
| J3 | GCT USB4110 USB-C | 16 + shield | VBUS, GND, CC1/2, D+/−, SBU1/2 | Bottom edge, cable access |
| J5 | 2.54 mm pin header | 4 | `3V3`, `SWDIO`, `SWCLK`, `PGND` | Top-right, vertical |
| J6 | 2.54 mm pin header | 3 | `3V3`, `OUT`, `PGND` | Top-right, vertical |
| J7 | 2.54 mm pin header | 2 | `USART1_TX`, `USART1_RX` | Top-right, vertical |

The three headers sit together on the upper-right quadrant so a single wiring harness can carry
programming, telemetry and the sensor; the two power connectors are on the lower edge where the
cables naturally leave the board.

### 8.2 Test points

| Ref | Net | Where to probe | Expected value |
|---|---|---|---|
| TP1 | `3V3` | next to the MCU | 3.30 V ±2 % |
| TP2 | `VIN` | bottom-right, after J1 | 12–24 V (supply voltage) |
| TP3 | `VIN_Prot` | centre-right, after Q1 | VIN − (I·R<sub>DS(on)</sub>), ≈VIN |
| TP4 | `5V_p` | bottom-left, after D6 | ≈4.6–4.8 V with USB |
| TP5 | `PGND` | centre | 0 V reference |
| TP6 | `5V_b` | left, buck output | 5.02 V ±2 % |
| TP7 | `PGND` | lower-centre | 0 V reference |
| TP8 | `5V_syst` | left, mux output | 5.0 V (buck) or ≈4.7 V (USB) |

All test points are 1.5 × 1.5 mm exposed pads on the front copper — probe them with a fine tip, not
a croc clip.

### 8.3 Indicators

| Ref | Circuit | Meaning |
|---|---|---|
| D1 | `3V3` → R3 1 kΩ → LED → `PGND` | 3.3 V rail alive; ≈1.3 mA |
| D3 | `ST` → LED → `PGND` | Intended as "buck is the active source"; see the §5.4 note — it has no current path or limiter as drawn |

---

## 9. Schematic walkthrough

The schematic is a single A3 sheet divided by text-box titles into eight blocks. Reading it in
order is the fastest way to understand the board.

| Block | Contents | What it does |
|---|---|---|
| **Input Protection** | J1, F1, Q1, D4, R15, D7, C25, C2, C9, C23 | Takes the raw motor supply, protects against reversal and transients, produces `VIN_Prot` |
| **Power** | Q2, R18, D1, R3, D3, TP1/TP5/TP7/TP8 | Load switch, LED, and the distribution of the muxed 5 V rail |
| **LMR36510ADDA** | U7, L1, C12, C10, R2, R4, C13, C14, C15, TP6 | 5 V buck |
| **TPS2116** | U8, R10, R14 | 5 V source selection |
| **TPS7A45** | U1, C3, C11, C8, C28, C18, C21, C19, C20 | 3.3 V LDO and its decoupling |
| **DRV8316** | U4, C26, C27, C24, R5, C29, R7, R8, R6 | Three-phase power stage and its straps |
| **STM32G4** | U2, SW4, C17, R1, J5, J6, J7 | MCU, reset, programming and interfaces |
| **USB Type-C** | J3, D5, D6, C16, R12, R13, TP4 | USB power and data entry |

**Signal-naming conventions.** Every net that crosses a block boundary has a global label, so the
sheet reads as a set of functional islands connected by name rather than by long wires. Power nets
(`VIN`, `VIN_Prot`, `5V_b`, `5V_p`, `5V_syst`, `3V3`, `PGND`) are the spine; the motor-facing nets
are `ph_A/B/C`; the driver-facing control nets are `INHA/INLA/…`, `SOA/SOB/SOC`, `FAULT_N`,
`DRVOFF`, `SLEEP_N`, `MODE`, `SLEW`.

**Why the blocks are ordered that way.** Power enters at the bottom-left of the sheet, is protected,
converted to 5 V, muxed, then converted to 3.3 V, and only then feeds the MCU and the driver's logic
side — the same left-to-right order as the physical layout, which is what makes the board easy to
trace with a probe.

---

## 10. PCB layout

### 10.1 Outline and mechanics

| Property | Value |
|---|---|
| Shape | Circle of radius 30 mm centred at (180, 90) mm, with a flat chord at y = 117.6 mm |
| Width | 60.0 mm (x = 150 … 210) |
| Height | 57.6 mm (y = 60 … 117.6) |
| Flat edge | 23.5 mm long (x = 168.24 … 191.76), 2.4 mm deep |
| Mounting holes | 4× 2.2 mm NPTH at (155, 78.5), (205, 78.5), (155, 101.5), (205, 101.5) |
| Hole pattern | 50 × 23 mm rectangle, 5 mm in from the rim |
| Thickness | 1.6 mm |
| Edge clearance | Copper and silkscreen kept 0.3–0.5 mm from the rim |

The flat is not cosmetic: it gives J1 and J3 a straight edge to sit against, so the screw terminal
and the USB receptacle do not overhang a curved rim. The four M2 holes are far enough inboard to
leave the motor-terminal copper and the USB pads untouched.

### 10.2 Stackup

| Layer | Use |
|---|---|
| F.Cu (1 oz) | Components, signal routing, `PGND` pour |
| FR-4 core (1.6 mm) | — |
| B.Cu (1 oz) | Signal routing, `PGND` pour |

Two layers with a full ground pour on both is the right trade for this power level: the return
currents of the phase outputs stay directly under their traces, and the DRV8316's thermal pad has
copper on both sides.

### 10.3 Floorplan

Coordinates are millimetres in the KiCad frame (y increases downward).

```
┌─────────────────────────────────── top arc  y = 60 ────────────────────────────────────┐
│    SW4 182,66            C17               C21                         J7 192,74       │
│                                                                        J6 201,72       │
│    C19                   U2 MCU 184,74             R6  C24             J5 196,80       │
│    U1 LDO 172,81           C8                    H1 155,78             H4 205,78       │
│    Q2 166,84         C3                U4 DRV8316 191,90               J2 205,85       │
│    R10 R14           C28                                               H3 205,101      │
│    U8 156,86           C13 C14 C15                                                     │
│    L1 161,101              C23 C26 C27 R5 C29                                          │
│    U7 172,101                                  D7 196,103                              │
│    C12 C10 R2 R4                           Q1 186,103                                  │
│    D5 D6 R12 R13                           F1 200,109                  H2 155,101      │
│      J3 172,113                              J1 188,115                                │
└───────────────────────────────── flat edge  y = 117.6 ─────────────────────────────────┘
```

**Zones, and why they are where they are:**

* **Bottom edge — power entry.** J1 and J3 are on the flat, so both cables leave downward and
  cannot foul the circular body. F1, Q1, D4, D7 and the `VIN_Prot` capacitors are immediately
  inboard of J1: the high-current path is short (J1 → F1 → Q1 → driver) and never crosses the
  sensitive analogue area.
* **Left half — power conversion.** U7, L1, C13/C14/C15 and the input capacitors occupy the left
  side, with the switching node confined to a small area around U7 pin 8. The TPS2116 and its
  divider sit just above, close to both 5 V sources. Keeping all switching copper on one side
  leaves the right half quiet.
* **Centre — linear post-regulation.** U1, Q2, C3, C8 sit in the middle, between the mux output and
  the MCU, so the 3.3 V trunk is short and the LDO's heat is spread over open copper rather than
  under a connector.
* **Right half — motor drive.** U4 is at the centre-right, with its `VIN_Prot` bulk capacitors
  (C23, C25), charge-pump capacitors (C26, C27) and buck network (R5, C29) clustered within a few
  millimetres of the pins they serve. J2 is on the right rim, so the three 1.3 mm phase traces run
  only about 14 mm from the package pins to the terminal.
* **Top — control and interfaces.** The MCU sits high and central, roughly equidistant from the
  driver (PWM and sense), the headers (J5/J6/J7 on the upper right) and the reset button (SW4 at the
  top). The USB data pair runs from J3 up the right of the LDO to PA11/PA12 without crossing the
  switching node.
* **Mounting holes** are clear of all copper except the ground pour, which is relieved around them.

### 10.4 Netclasses, track widths and vias

Project netclass `Default`: 0.2 mm track, 0.2 mm clearance, via 0.6/0.3 mm, 0.25 mm hole-to-hole.
The custom rules file is empty, so every check is a KiCad default.

**Track widths actually used (524 segments):**

| Width | Segments | Used for |
|---|---|---|
| 0.20 mm | 259 | Logic, PWM, sense, USB, straps |
| 0.25 mm | 28 | Reset, UART, SWD, LED, PR1 divider |
| 0.30 mm | 34 | Short power stubs |
| 0.40 mm | 87 | `3V3`, `VBUS`, `5V_p`, buck feedback/boot/VCC |
| 0.60 mm | 7 | Gate/clamp and `D4-A` |
| 0.65 mm | 11 | `5V_b`, `5V_syst`, LDO input |
| 0.70 mm | 8 | Buck output / input links |
| 1.00 mm | 37 | `VIN_Prot`, buck switch node, phase stubs |
| 1.20 mm | 6 | `VIN_Fuse` |
| 1.30 mm | 47 | `VIN`, `ph_A`, `ph_B`, `ph_C` |

**Layer usage:** 389 segments on F.Cu, 135 on B.Cu. The bottom layer is used mainly for the return
paths, the USB pair's escape, and cross-connections that would otherwise cut the front pour.

**Vias (300 total):**

| Size / drill | Count | Use |
|---|---|---|
| 0.60 / 0.30 mm | 165 | Power and ground stitching, thermal |
| 0.50 / 0.30 mm | 135 | Signal layer changes, ground stitching |

**Via distribution by net:** `PGND` 174, `VIN_Prot` 41, `ph_A` 11, `ph_C` 9, `ph_B` 8, `VIN_Fuse` 7,
buck `SW` 5, `5V_b` 4, `SOC` 4, `SOA` 4, `SOB` 3, `VBUS` 3, and single/double vias on the remaining
signals. The high `PGND` count is deliberate: it is what ties the two ground pours into one
electrical surface.

### 10.5 Ground strategy

There is exactly **one** ground net, `PGND`, poured on both layers:

| Zone | Layer | Settings |
|---|---|---|
| `PGND_F` | F.Cu | clearance 0.2 mm, min thickness 0.4 mm, thermal gap 0.5 mm, thermal bridge 0.5 mm |
| `PGND_B` | B.Cu | same |

A single net (rather than split analogue/power grounds) is the correct choice here: the DRV8316's
`AGND` and `PGND` pins are tied together at the package, the current-sense amplifiers are internal
and referenced to the same ground, and the MCU's `VSSA` is a separate pin but the same net. The
risk with a split plane would be return currents finding a detour; with one poured surface and 174
stitching vias, the return path under any trace is never more than a via away.

Thermal reliefs (0.5 mm gap, 0.5 mm spoke) are used on the pour, which keeps hand-soldering
practical on the SMD pads while still giving a low-impedance connection at the via-stitched areas.

### 10.6 Thermal design

| Heat source | Path | Comment |
|---|---|---|
| U4 DRV8316 | Exposed pad 41 → `PGND` land → both pours | The dominant heat source once the motor is loaded; 95 mΩ × I² conduction plus switching |
| Q1 | LFPAK56 drain tab → `VIN_Prot` copper | ≈0.3 W at 3 A |
| U1 | SOT-223 tab (pin 3) → `PGND` | ≈0.06 W at the logic load |
| U7 | HTSOP-8 pad → `PGND` | ≈0.1 W at 40 mA |

The DRV8316's dissipation is easy to estimate: with two phases conducting at any instant and
R<sub>DS(on)</sub> = 47.5 mΩ per FET, P ≈ 2·I²·0.0475. At 2 A RMS that is 0.38 W; at 5 A it is
2.4 W. The package's thermal pad and the double-sided pour handle the first comfortably; the second
needs airflow or duty-cycle limiting.

### 10.7 Routing notes

* **Phase outputs** leave the driver on 1.3 mm traces, run parallel and closely spaced to J2, and
  have the ground pour underneath — the loop area is small, which keeps the 125 V/µs edges from
  radiating.
* **Buck switch node** (`U7-SW`) is a 1.0 mm trace confined to the few millimetres between U7 pin 8,
  C12 and L1. It is the noisiest node on the board and is deliberately kept away from the analogue
  area and the USB pair.
* **Current-sense traces** (`SOA/SOB/SOC`) are 0.2 mm, short, and run from the driver's right-hand
  pins up to the MCU without crossing the phase traces; each is accompanied by pour on the far side.
* **USB pair** (`D+`/`D−`) is routed as a pair on 0.2 mm traces with the ESD array (D5) placed right
  at the connector, before the pair turns up to the MCU.
* **`VIN_Prot` distribution** is a 1.0 mm spine from Q1 across to the driver's `VM` pins, with the
  bulk capacitors tapped along it rather than at the end.
* **Strap resistors** (R7, R8) sit immediately beside pins 34 and 33 so the analogue levels are set
  at the pin, not after a long trace.

### 10.8 Silkscreen

| Text | Position | Purpose |
|---|---|---|
| `BLDC MOTOR CONTROLLER` + team names and roll numbers | Top arc, front | Board identification |
| `VIN +`, `PGND` | At J1 | Supply polarity |
| `A`, `B`, `C` | At J2 | Motor phase order |
| `3V3`, `PGND`, `OUT` | At J6 | Sensor pinout |
| `TX`, `RX` | At J7 | UART direction |
| `3V3`, `SWDIO`, `SWCLK`, `PGND` | At J5 | Programming pinout |
| `USB` | At J3 | Connector identity |
| `5V_b` | Near TP6 | Rail label |
| `${REFERENCE}` | Every footprint | Reference designators (ICs print their part name) |

### 10.9 DRC summary

| Check | First run | After clean-up | Meaning |
|---|---|---|---|
| `holes_co_located` | 82 | 82 | Two or more drill hits at one coordinate; dominated by the DRV8316 pad's via field, where 13 vias share the pad centre |
| `silk_over_copper` | 45 | 0 | Silkscreen printed over exposed copper |
| `silk_overlap` | 38 | 0 | Overlapping silkscreen text/graphics |
| `silk_edge_clearance` | 10 | 0 | Silkscreen too close to the board edge |
| `lib_footprint_mismatch` | 7 | 9 | Footprint differs from the library version (expected: custom DRV8316 footprint and hand-solder variants) |
| `hole_clearance` | 4 | 4 | Drill-to-drill or drill-to-copper spacing below the default rule |
| `via_dangling` | 4 | 4 | A via connected on only one layer |
| `track_dangling` | 1 | 1 | A track end not connected to anything |
| **Total** | **191** | **100** | |

There are **no clearance, short-circuit or unconnected-item errors** in either run — the electrical
connectivity is complete (the router reports zero unconnected items), and all remaining findings
are mechanical/silkscreen/duplicate-drill categories that do not affect function. The duplicate
drill hits appear as repeated coordinates in the PTH drill file; fabrication CAM merges them into a
single hole.

### 10.10 Fabrication outputs

| File | Content |
|---|---|
| `*-F_Cu.gbr`, `*-B_Cu.gbr` | Copper, 2 layers |
| `*-F_Mask.gbr`, `*-B_Mask.gbr` | Solder mask |
| `*-F_Paste.gbr`, `*-B_Paste.gbr` | Paste (stencil) |
| `*-F_Silkscreen.gbr`, `*-B_Silkscreen.gbr` | Legend |
| `*-Edge_Cuts.gbr` | Outline |
| `*-PTH.drl` | Plated holes: 0.30, 0.33, 1.00, 1.01, 1.30 mm |
| `*-NPTH.drl` | Non-plated: 0.65 mm (USB shield slots), 2.2 mm (mounting) |
| `*-job.gbrjob` | Gerber job file |

`fyp_bldc_v7_grbr.zip` bundles the same set; the files inside still carry the `v6` name, which is
harmless but worth renaming before sending to a fab if traceability matters.

---

## 11. Assembly and bring-up

### 11.1 Assembly order

1. **SMD first, bottom to top by height.** Start with the QFN/LQFP parts (U4, U2), then the
   SOT/HTSOP parts (U1, U7, U8, Q1, Q2, D5), then passives, then the LEDs and the button.
2. **Stencil and reflow** for the SMD side; the DRV8316 and the MCU both need paste on the exposed
   pads as well as the pins.
3. **Through-hole last:** J1, J2, J3 (shield tabs), J5, J6, J7, F1. The USB-C shield tabs need
   enough solder to fill the 0.65 mm slots.
4. **Inspection:** check for bridging on the LQFP-48 (0.5 mm pitch) and on the DRV8316's 0.5 mm
   pads, and confirm the thermal pad is wetted (a void there shows up as a hot spot later).

### 11.2 Bring-up procedure

Do this with **no motor connected** and a current-limited supply.

| Step | Action | Expected |
|---|---|---|
| 1 | Visual check, continuity from `VIN` to `PGND` | > 10 kΩ (no short) |
| 2 | Apply 12 V through a 100 mA limit, measure TP2 | 12 V |
| 3 | Measure TP3 (`VIN_Prot`) | ≈12 V minus a few tens of mV |
| 4 | Measure TP6 (`5V_b`) | 5.02 V ±2 % |
| 5 | Measure TP8 (`5V_syst`) | ≈5.0 V (buck source) |
| 6 | Measure TP1 (`3V3`) | 3.30 V ±2 %; D1 lit |
| 7 | Check U4 `AVDD` (C24 pad) | ≈3.3 V (internal regulator, not the board rail) |
| 8 | Connect SWD, read the device ID | Cortex-M4F responds; no lock-up |
| 9 | With firmware loaded, hold `DRVOFF` high, then release | Driver leaves Hi-Z; `nFAULT` stays high |
| 10 | Connect a small motor, low duty, check all three phases on a scope | Complementary PWM, ≈1 µs dead time, 125 V/µs edges |
| 11 | Unplug `VIN` with USB connected | TP8 stays ≈4.7 V; TP1 stays 3.30 V |
| 12 | Re-apply `VIN` | TP8 returns to 5.0 V with no 3.3 V glitch |

**If something is wrong:**

* No `5V_b` → check U7 EN (should equal `VIN_Prot`), the `BOOT` cap, and that L1 is fitted.
* `5V_b` present but no `3V3` → check Q2's gate is low and U1 pin 1 is tied to `U1-IN`.
* `3V3` collapses when the motor starts → check the `5V_syst` handover and the LDO's input bulk.
* `nFAULT` low with no motor → check `DRVOFF`, `nSLEEP`, and that `MODE`/`SLEW`/`GAIN` straps are
  the intended levels.

### 11.3 Handling notes

* The DRV8316's `VM` is rated 40 V absolute maximum — never hot-plug a 24 V supply without the TVS
  fitted, and keep the input below 35 V.
* The board has no reverse-current path from the motor back into the supply other than the FET body
  diodes; do not back-drive the motor faster than the supply can absorb without a brake strategy.
* `VIN_Prot` stays charged for a moment after power-off (C23 + C9 ≈ 12 µF); discharge it before
  reworking Q1 or U4.

---

## 12. Firmware interface

The firmware is maintained separately from this repository. This section defines the contract the
hardware imposes on it.

### 12.1 Peripheral map

| Function | Peripheral | Pins | Configuration notes |
|---|---|---|---|
| Motor PWM | TIM1 CH1/2/3 + CH1N/2N/3N | PA8/PA9/PA10 + PB13/PB14/PB15 | Complementary, centre-aligned, dead time, MOE |
| Phase A current | ADC1_IN1 | PA0 | Injected group, timer-triggered |
| Phase B current | ADC1_IN2 | PA1 | Injected group |
| Phase C current | ADC1_IN3 | PA2 | Injected group |
| DC link (optional) | — | — | **No `VM` divider on this board**; bus voltage is not measurable without a rework |
| Driver fault | EXTI + optionally TIM1_BKIN | PA6 | Falling edge; PA6 is TIM1_BKIN (AF6) |
| Driver enable | GPIO | PC13 | High = awake; 20–40 µs low pulse clears a fault |
| Driver output disable | GPIO | PA7 | High = all FETs off |
| Sensor input | TIM2_CH1 input capture | PA15 | PWM/frequency measurement |
| Telemetry link | USART1 | PB6 (TX), PB7 (RX) | 3.3 V levels, no flow control |
| Debug | SWD | PA13/PA14 | — |
| USB | USB FS device | PA11/PA12 | HSI48 + CRS required |

### 12.2 PWM generation

The driver is strapped for **6× PWM**, so the MCU owns the dead time.

* **Timer:** TIM1, centre-aligned (up-down) counting. Centre alignment halves the current ripple
  for the same switching frequency and puts the ADC trigger at the counter peak, away from the
  switching edges.
* **Frequency:** at 170 MHz, a 20 kHz centre-aligned PWM gives an ARR of about 4250 counts
  (≈12.1 bits of duty resolution); 40 kHz halves that. 20 kHz is a good default for a 24 V machine.
* **Dead time:** with `SLEW` at 125 V/µs, the driver's own dead time is 650–1000 ns and its
  propagation delay ≈1.1 µs. Program the timer's dead time to **≈1 µs** (DTG ≈ 170 counts at
  170 MHz) and never below the driver's minimum.
* **Channel mapping:** phase C = CH1, phase B = CH2, phase A = CH3 (see §7.2). Getting this wrong
  produces a motor that stutters or runs backwards.
* **Safety:** set `DRVOFF` (PA7) high before configuring the timer, enable MOE only when the loop is
  ready, and clear MOE on any fault. Optionally route `nFAULT` into TIM1_BKIN so a driver fault
  shuts the PWM off in hardware rather than in an interrupt.

### 12.3 Current sensing

* Three injected conversions on ADC1 (channels 1, 2, 3), triggered by TIM1's TRGO at the counter
  peak (or at the underflow, depending on where the low-side shunts are sampled — the DRV8316 senses
  low-side current, so sample when the low-side FETs are on).
* Sample time: the source impedance is the driver's internal amplifier output, so a short sample
  window (≈12–24 cycles at 170 MHz) is enough; longer windows reduce noise.
* **Offset calibration:** at zero current, read all three channels and store the mid-scale values.
  Subtract them in the Clarke transform. The datasheet allows ±50 mA of offset, which is a
  meaningful fraction of a 3 A phase current.
* **Scaling:** 0.3 V/A with `VREF` = `VREF+` = 3.3 V, so `I = (V_SOx − V_offset) / 0.3`.
* **Reconstruction:** in 6× PWM with three shunts, each phase current is valid only during its
  low-side conduction window. Either sample at the PWM centre where all three low sides are on
  (only possible with a zero vector) or reconstruct the third current from Kirchhoff's law and
  sample two phases.

### 12.4 Control loop

A conventional sensorless FOC chain, run from the ADC end-of-injection interrupt:

```
ADC (Ia, Ib, Ic) → Clarke → Park(θ) → PI(Id) / PI(Iq) → inverse Park → SVM → TIM1 CCRx
                                  ▲
                     θ from flux observer / BEMF PLL
```

The CORDIC and FMAC blocks in the G4 can offload the sin/cos and the vector rotation; at 20 kHz
there is ample CPU time even with plain `arm_sin_cos_f32`.

**Startup.** A sensorless machine cannot estimate the rotor angle at standstill. The usual sequence
is: align (force a known vector for a few hundred milliseconds), then open-loop ramp, then hand over
to the observer once BEMF is measurable. This is exactly the phase where the AS5600 approach in §13
helps.

### 12.5 UART protocol (suggested)

The hardware only provides a 2-wire UART, so the protocol is a firmware choice. A compact binary
frame works well over a wireless link:

| Field | Size | Notes |
|---|---|---|
| Sync | 2 B | `0xAA 0x55` |
| Length | 1 B | Payload length |
| Command | 1 B | `0x01` set duty, `0x02` set speed, `0x03` set Iq, `0x04` stop, `0x10` telemetry |
| Payload | n B | Little-endian `float` or `int16` |
| CRC | 1 B | CRC-8 over length..payload |

Telemetry at 50–100 Hz carrying `Iq`, `Id`, electrical angle, speed and fault flags is enough for a
dashboard. The ESP32 side owns Wi-Fi/BLE and simply forwards frames.

### 12.6 Fault handling

| Event | Detection | Response |
|---|---|---|
| Over-current / OCP | `nFAULT` low | Clear MOE, `DRVOFF` high, log, retry after ≥5 ms (driver retry is 4–6 ms) |
| Thermal warning/shutdown | `nFAULT` low | Same, plus reduce current limit |
| Undervoltage | `nFAULT` low | Stop, wait for recovery |
| MCU-side fault | — | `DRVOFF` high is the fastest software kill |
| Fault latch reset | — | Pulse `nSLEEP` low for 20–40 µs (do **not** exceed 120 µs or the part sleeps) |

Because the T variant has no SPI, there is no fault register to read: the only diagnostic is the
`nFAULT` line plus the firmware's own measurements. Design the fault handler to be conservative.

---

## 13. Position sensing and sensored FOC (AS5600)

### 13.1 What the sensor is

The AS5600 is a 12-bit magnetic rotary position sensor: a diametric magnet on the shaft, a Hall
front end, and an output stage that can present the angle as

* an **I²C** register value (address `0x36`), 12 bits, or
* a **ratiometric analogue voltage**, or
* a **PWM** pulse train whose duty cycle encodes the angle.

It runs from 3.3 V and draws a few milliamps, which is within what J6 can supply.

### 13.2 How it connects here

J6 is a three-pin port: `3V3` / `OUT` / `PGND`. That matches the AS5600's three-wire mode exactly,
and PA15 (TIM2_CH1) is a timer input, so the PWM output can be captured with no extra hardware.

| Mode | Wiring on J6 | MCU resource | Status on this board |
|---|---|---|---|
| PWM output | 3V3, OUT, GND | PA15 / TIM2_CH1 input capture | **Works as built** |
| Analogue output | 3V3, OUT, GND | needs an ADC pin | **Does not work as built** — PA15 has no ADC channel |
| I²C | 3V3, SDA, SCL (+GND) | PB10/PB11 | Possible, but I²C is not brought to any header |

### 13.3 The PWM path (works today)

1. Configure the AS5600's `OUTS` field (CONF register, over I²C once) so `OUT` emits PWM rather than
   an analogue level. The default output mode is not guaranteed to be PWM, so the sensor must be
   configured before first use.
2. Capture on TIM2_CH1: measure both the high time and the period, and compute
   `angle = 360° × t_high / t_period`. Using the measured period rather than a nominal one removes
   the sensor's clock tolerance.
3. Resolution is set by the sensor's PWM frame rate and the timer clock. At 170 MHz a 1 µs high
   time is 170 counts, so the duty measurement is far more precise than the sensor's 12 bits; the
   real limit is the PWM update rate (hundreds of hertz), i.e. latency, not resolution.

**Latency is the catch.** A PWM frame takes on the order of a millisecond, which at 20 kHz control
rate is 20 loops old. For low-speed sensored operation (the usual reason to fit an encoder) that is
acceptable; for high-speed field weakening it is not.

### 13.4 Making the analogue path work

PA15 is a plain digital/timer pin. To use the AS5600's analogue output, J6 pin 2 must be rerouted to
an ADC-capable pin. The free, ADC-capable pins on this board are:

| Pin | Port | ADC channel | Note |
|---|---|---|---|
| 11 | PA3 | ADC1_IN4 | labelled net already present |
| 13 | PA5 | ADC2_IN13 | labelled net already present |
| 16 | PB0 | ADC1_IN15 | labelled net already present |
| 17 | PB1 | ADC1_IN12 | labelled net already present |
| 18 | PB2 | ADC2_IN12 | labelled net already present |
| 26 | PB12 | ADC1_IN11 | labelled net already present |

A flying lead from J6 pin 2 to one of those pads (or a footprint rework in the next revision) gives a
direct, low-latency ratiometric angle — the cleanest option if the sensor is kept.

### 13.5 The I²C path

I²C1 is wired to PB10 (SCL, pin 22) and PB11 (SDA, pin 25) with global labels, but there is no
header and no pull-up resistors on the board. Using it requires two flying wires plus 2.2–4.7 kΩ
pull-ups to 3.3 V. In exchange you get the full 12-bit absolute angle with a register read
(≈100 µs at 400 kHz), which is the best latency/accuracy combination of the three options.

### 13.6 Why sensored, and what it changes

Sensorless FOC needs a back-EMF estimate, so it cannot start from rest and degrades at very low
speed. A absolute angle sensor removes both limitations:

* no open-loop alignment/ramp — the first PWM cycle can be a real FOC vector;
* full torque at zero speed (subject to the current limit and thermal budget);
* the observer becomes optional, so the firmware can fall back to sensorless if the magnet or
  sensor fails.

The board was built with the sensorless path as the primary mode and the AS5600 experiment as a
secondary path on J6; the hardware supports the PWM variant without modification and the other two
variants with the small reworks described above.

---

## 14. Bill of materials

67 components plus 4 mounting holes = 71 footprints. `DNP` is empty for every line — everything in
the table is fitted.

### 14.1 Capacitors (23)

| Ref | Qty | Value | Package | Function |
|---|---|---|---|---|
| C2 | 1 | 220 nF | 0805 | `VIN_Prot` mid-frequency decoupling |
| C3 | 1 | 100 µF | 0805 | LDO input bulk |
| C8 | 1 | 10 µF | 0805 | LDO output bulk (stability minimum) |
| C9 | 1 | 2.2 µF | 0805 | Buck input bulk |
| C10 | 1 | 1 µF | 0805 | LMR36510 `VCC` |
| C11 | 1 | 1 µF | 0805 | LDO input HF decoupling |
| C12 | 1 | 100 nF | 0805 | Buck bootstrap |
| C13, C14 | 2 | 22 µF | 1206 | `5V_b` output bulk |
| C15 | 1 | 1 µF | 0805 | `5V_b` HF decoupling |
| C16 | 1 | 1 µF | 0805 | `5V_p` bulk |
| C17 | 1 | 0.1 µF | 0805 | NRST filter |
| C18 | 1 | 0.1 µF | 0805 | `3V3` MCU decoupling |
| C19 | 1 | 1 µF | 0805 | `3V3` MCU decoupling |
| C20 | 1 | 1 µF | 0805 | `3V3` MCU decoupling |
| C21 | 1 | 0.1 µF | 0805 | `3V3` MCU decoupling |
| C23 | 1 | 10 µF | 1210 | `VIN_Prot` bulk at the driver |
| C24 | 1 | 0.1 µF | 0805 | DRV8316 `AVDD` |
| C25 | 1 | 0.1 µF | 0805 | `VIN_Prot` HF decoupling |
| C26 | 1 | 1 µF | 0805 | Charge-pump reservoir |
| C27 | 1 | 47 nF | 0805 | Charge-pump flying capacitor |
| C28 | 1 | 0.1 µF | 0805 | `3V3` HF decoupling |
| C29 | 1 | 22 µF | 1206 | DRV8316 buck output (unused rail) |

### 14.2 Resistors (14)

| Ref | Qty | Value | Package | Function |
|---|---|---|---|---|
| R1 | 1 | 10 kΩ | 0805 | PB9 pull-down |
| R2 | 1 | 100 kΩ | 0805 | Buck feedback top |
| R3 | 1 | 1 kΩ | 0805 | D1 series resistor |
| R4 | 1 | 24.9 kΩ | 0805 | Buck feedback bottom |
| R5 | 1 | 22 Ω | 0805 | DRV8316 buck resistor mode |
| R6 | 1 | 5.1 kΩ | 0805 | `nFAULT` pull-up |
| R7 | 1 | 47 kΩ | 0805 | `SLEW` strap (to `AVDD`) |
| R8 | 1 | 0 Ω | 0805 | `MODE` strap (to `PGND`) |
| R10 | 1 | 30 kΩ | 0805 | TPS2116 `PR1` divider top |
| R12, R13 | 2 | 5.1 kΩ | 0805 | USB-C CC termination (to `3V3`) |
| R14 | 1 | 10 kΩ | 0805 | TPS2116 `PR1` divider bottom |
| R15 | 1 | 100 kΩ | 0805 | Q1 gate pull-down |
| R18 | 1 | 100 kΩ | 0805 | Q2 gate pull-down |

### 14.3 Semiconductors and passives (16)

| Ref | Qty | Value / Part | Package | Function |
|---|---|---|---|---|
| D1, D3 | 2 | LED | 0603 | 3.3 V indicator; mux status |
| D4 | 1 | `D_Zener` (value not in BOM) | SOD-123 | Q1 gate clamp |
| D5 | 1 | SP0503BA | SOT-143 | USB ESD array |
| D6 | 1 | SS14 | SMA | USB reverse-current blocking |
| D7 | 1 | `D_TVS` (value not in BOM) | SMB | Input transient clamp |
| F1 | 1 | MF-RHT500 PTC | radial | Input over-current protection |
| L1 | 1 | 22 µH | SRN6028 | Buck inductor |
| Q1 | 1 | BUK6Y33-60PX | LFPAK56 | Reverse-polarity P-FET |
| Q2 | 1 | AO3407A | SOT-23 | 5 V load switch |
| SW4 | 1 | Alps SKRK SPST-NO | SMD | Reset button |
| TP1–TP8 | 8 | test point | 1.5 mm pad | `3V3`, `VIN`, `VIN_Prot`, `5V_p`, `PGND`×2, `5V_b`, `5V_syst` |

### 14.4 Integrated circuits (5)

| Ref | Qty | Part | Package | Function |
|---|---|---|---|---|
| U1 | 1 | TPS7A4533 | SOT-223-6 | 3.3 V / 1.5 A LDO |
| U2 | 1 | STM32G431CBT6 | LQFP-48 | Cortex-M4F MCU |
| U4 | 1 | DRV8316 (T fitted; BOM says DRV8316RRGFR) | VQFN-40 7×5 | Three-phase driver |
| U7 | 1 | LMR36510ADDA | HTSOP-8 | 5 V buck |
| U8 | 1 | TPS2116DRL | SOT-583-8 | Power mux |

### 14.5 Connectors and mechanical (10)

| Ref | Qty | Part | Function |
|---|---|---|---|
| J1 | 1 | Phoenix PT-1.5-2-5.0-H | VIN / PGND terminal |
| J2 | 1 | Phoenix PT-1.5-3-5.0-H | Motor A/B/C terminal |
| J3 | 1 | GCT USB4110 USB-C 16P | USB power and data |
| J5 | 1 | 1×04 2.54 mm header | SWD |
| J6 | 1 | 1×03 2.54 mm header | Sensor |
| J7 | 1 | 1×02 2.54 mm header | UART |
| H1–H4 | 4 | M2 mounting hole, 2.2 mm | Mechanical |

---

## 15. Netlist appendix

Complete connectivity as extracted from the PCB database. Node notation is `reference.pad`.

### 15.1 Power and ground

| Net | Nodes | Connections |
|---|---|---|
| `PGND` | 57 | C2.1, C3.2, C8.2, C9.1, C10.2, C11.2, C13.1, C14.1, C15.1, C16.2, C18.1, C19.1, C20.1, C21.1, C23.1, C24.2, C25.1, C28.1, C29.2, D1.1, D3.1, D5.1, D7.2, DRV8316.2/4/12/15/18/24/26/41, J1.2, J3.A1/A12/B1/B12 + shield, J5.4, J6.3, LMR3651.1/4/9 + pad, R1.1, R4.2, R8.1, R14.2, R15.2, R18.1, STM32G431x.19/23/35/47, TP5.1, TP7.1, TPS2116.1, TPS7A45.3 |
| `3V3` | 24 | C8.1, C17.1, C18.2, C19.2, C20.2, C21.2, C28.2, DRV8316.37, J5.1, J6.1, R3.2, R6.1, R12.2, R13.2, STM32G431x.1/20/21/24/36/48, SW4.1, TP1.1, TPS7A45.4/5 |
| `VIN` | 3 | F1.1, J1.1, TP2.1 |
| `VIN_Fuse` | 3 | D4.1, F1.2, Q1.2 |
| `VIN_Prot` | 13 | C2.2, C9.2, C23.2, C25.2, C26.2, D7.1, DRV8316.9/10/11, LMR3651.2/3, Q1.3, TP3.1 |
| `5V_b` | 9 | C13.2, C14.2, C15.2, L1.2, R2.2, R10.2, TP6.1, TPS2116.3, TPS2116.5 |
| `5V_p` | 4 | C16.1, D6.1, TP4.1, TPS2116.6 |
| `5V_syst` | 4 | Q2.2, TP8.1, TPS2116.2, TPS2116.7 |
| `VBUS` | 6 | D5.4, D6.2, J3.A4/A9/B4/B9 |

### 15.2 Motor and driver interface

| Net | Nodes | Connections |
|---|---|---|
| `ph_A` | 3 | DRV8316.13, DRV8316.14, J2.3 |
| `ph_B` | 3 | DRV8316.16, DRV8316.17, J2.2 |
| `ph_C` | 3 | DRV8316.19, DRV8316.20, J2.1 |
| `INHA` | 2 | DRV8316.27, STM32G431x.32 |
| `INLA` | 2 | DRV8316.28, STM32G431x.29 |
| `INHB` | 2 | DRV8316.29, STM32G431x.31 |
| `INLB` | 2 | DRV8316.30, STM32G431x.28 |
| `INHC` | 2 | DRV8316.31, STM32G431x.30 |
| `INLC` | 2 | DRV8316.32, STM32G431x.27 |
| `SOA` | 2 | DRV8316.40, STM32G431x.8 |
| `SOB` | 2 | DRV8316.39, STM32G431x.9 |
| `SOC` | 2 | DRV8316.38, STM32G431x.10 |
| `FAULT_N` | 3 | DRV8316.22, R6.2, STM32G431x.14 |
| `DRVOFF` | 2 | DRV8316.21, STM32G431x.15 |
| `SLEEP_N` | 2 | DRV8316.23, STM32G431x.2 |
| `MODE` | 2 | DRV8316.33, R8.2 |
| `SLEW` | 2 | DRV8316.34, R7.2 |

### 15.3 Interfaces

| Net | Nodes | Connections |
|---|---|---|
| `D+` | 4 | D5.3, J3.A6, J3.B6, STM32G431x.34 |
| `D-` | 4 | D5.2, J3.A7, J3.B7, STM32G431x.33 |
| `CC1` | 2 | J3.A5, R12.1 |
| `CC2` | 2 | J3.B5, R13.1 |
| `SWDIO` | 2 | J5.2, STM32G431x.37 |
| `SWCLK` | 2 | J5.3, STM32G431x.38 |
| `USART1_TX` | 2 | J7.1, STM32G431x.43 |
| `USART1_RX` | 2 | J7.2, STM32G431x.44 |
| `OUT` | 2 | J6.2, STM32G431x.39 |
| `I2C1_SCL` | 1 | STM32G431x.22 |
| `I2C1_SDA` | 1 | STM32G431x.25 |

### 15.4 Internal (auto-named) nets

| Net | Nodes | Connections |
|---|---|---|
| `Net-(D1-A)` | 2 | D1.2, R3.1 |
| `Net-(D3-A)` | 2 | D3.2, TPS2116.8 |
| `Net-(D4-A)` | 4 | D4.2, Q1.1 + gate pads, R15.1 |
| `Net-(Q2-~-Pad1)` | 2 | Q2.1, R18.2 |
| `Net-(U1-IN)` | 5 | C3.1, C11.1, Q2.3, TPS7A45.1, TPS7A45.2 |
| `Net-(U2-PG10)` | 3 | C17.2, STM32G431x.7, SW4.2 |
| `Net-(U2-PB9)` | 2 | R1.2, STM32G431x.46 |
| `Net-(U4-AVDD)` | 3 | C24.1, DRV8316.25, R7.1 |
| `Net-(U4-CP)` | 2 | C26.1, DRV8316.8 |
| `Net-(U4-CPH)` | 2 | C27.1, DRV8316.7 |
| `Net-(U4-CPL)` | 2 | C27.2, DRV8316.6 |
| `Net-(U4-FB_BK)` | 3 | C29.1, DRV8316.3, R5.1 |
| `Net-(U4-SW_BK)` | 2 | DRV8316.5, R5.2 |
| `Net-(U7-BOOT)` | 2 | C12.1, LMR3651.7 |
| `Net-(U7-FB)` | 3 | LMR3651.5, R2.1, R4.1 |
| `Net-(U7-SW)` | 3 | C12.2, L1.1, LMR3651.8 |
| `Net-(U7-VCC)` | 2 | C10.1, LMR3651.6 |
| `Net-(U8-PR1)` | 3 | R10.1, R14.1, TPS2116.4 |
| `PB9` (label) | 1 | STM32G431x.45 (BOOT0 pad) |
| `PA3`, `PA5`, `PB0`, `PB1`, `PB2`, `PB12` | 1 each | Spare MCU pins, no destination |

### 15.5 Unconnected pins

| Net | Node |
|---|---|
| `unconnected-(U4-NC-Pad1)` | DRV8316.1 |
| `unconnected-(U4-SCLK-Pad35)` | DRV8316.35 (`OCP/SR` on the T part — Hi-Z level) |
| `unconnected-(U4-SCS_N-Pad36)` | DRV8316.36 (`GAIN` on the T part — Hi-Z level) |
| `unconnected-(U2-PA4-Pad12)` | STM32 pin 12 |
| `unconnected-(U2-PC14-Pad3)`, `…PC15-Pad4` | STM32 pins 3, 4 |
| `unconnected-(U2-PF0-Pad5)`, `…PF1-Pad6` | STM32 pins 5, 6 |
| `unconnected-(U2-PB3-Pad40)`, `…PB4-Pad41`, `…PB5-Pad42` | STM32 pins 40, 41, 42 |
| `unconnected-(J3-SBU1-PadA8)`, `…SBU2-PadB8` | USB-C sideband pins |

---

## 16. Design calculations

### 16.1 Buck output

```
Vout = 1.0 V × (1 + 100k / 24.9k) = 5.016 V
Worst case with 1 % resistors: 4.93 … 5.10 V
```

### 16.2 Buck inductor ripple

```
D = Vout/Vin,  ΔIL = Vout × (1 − D) / (L × fsw)
24 V in: D = 0.209, ΔIL = 5 × 0.791 / (22e-6 × 400e3) = 0.45 A
12 V in: D = 0.418, ΔIL = 5 × 0.582 / (22e-6 × 400e3) = 0.33 A
```

Peak inductor current at 1 A load ≈ 1.23 A, below the LMR36510's current limit.

### 16.3 Mux switchover threshold

```
PR1 = 5V_b × 10k/(30k + 10k) = 0.25 × 5V_b
Switch when PR1 = VREF = 1.0 V  →  5V_b = 4.0 V
At 5.0 V: PR1 = 1.25 V (25 % margin above threshold)
```

### 16.4 LDO dissipation

```
P = (Vin − Vout) × Iout = (5.0 − 3.3) × 0.035 = 0.06 W
Junction rise (SOT-223, θJA ≈ 60 °C/W) ≈ 3.6 °C
```

### 16.5 Current-sense scaling

```
I_phase = (V_SOx − V_offset) / 0.3 V/A
Linear output window: 0.25 V … 3.05 V  (VREF = 3.3 V)
Usable bidirectional range about the mid-scale bias: ≈ ±4.6 A
Full-scale (analogue) ±8 A; the ADC clips first
```

### 16.6 Driver conduction loss

```
P ≈ 2 × I² × R_DS(on)_per_FET,  R_DS(on)_per_FET = 95 mΩ / 2 = 47.5 mΩ
2 A RMS: 2 × 4 × 0.0475 = 0.38 W
3 A RMS: 2 × 9 × 0.0475 = 0.86 W
5 A RMS: 2 × 25 × 0.0475 = 2.38 W
```

### 16.7 PTC derating

```
I_hold(23 °C) = 5.0 A,  I_trip = 9.0 A
I_hold(70 °C) = 3.33 A
V_max = 16 V  → see the §5.1 note for 24 V operation
```

### 16.8 Track current (IPC-2221, 1 oz, 10 °C rise, external)

```
1.0 mm ≈ 2.5 A      1.3 mm ≈ 3.3 A
Phase outputs use 1.3 mm; VIN_Prot uses 1.0 mm
```

### 16.9 PWM resolution

```
f_pwm = 170 MHz / (2 × ARR)   (centre-aligned)
20 kHz → ARR ≈ 4250 → 12.1 bits
40 kHz → ARR ≈ 2125 → 11.1 bits
Dead time 1 µs → DTG ≈ 170 counts at 170 MHz
```

---

## 17. Glossary

| Term | Meaning |
|---|---|
| **BLDC** | Brushless DC motor; electronically commutated permanent-magnet machine |
| **FOC** | Field-oriented control; control of stator current in the rotor reference frame |
| **PMSM** | Permanent-magnet synchronous motor (FOC applies to both) |
| **BEMF** | Back-electromotive force; the voltage the spinning rotor induces in the windings |
| **SVM** | Space-vector modulation; the standard way to synthesise three-phase voltages from a DC link |
| **Clarke / Park** | Transforms from three-phase quantities to αβ, then to the rotating dq frame |
| **Dead time** | The gap inserted between turning one half-bridge FET off and the other on, to prevent shoot-through |
| **Shoot-through** | A simultaneous conduction of both FETs in a half-bridge, shorting the DC link |
| **MOE** | Main output enable (TIM1 bit); clearing it forces all PWM outputs to their inactive state |
| **BKIN** | Break input; a hardware path that clears MOE on a fault |
| **HS / LS** | High-side / low-side FET of a half-bridge |
| **VM** | Motor supply pin of the driver |
| **AVDD** | The DRV8316's internal 3.3 V regulator output (not the board's 3V3) |
| **CSA** | Current-sense amplifier |
| **VREF** | The reference voltage that sets the CSA output bias and the ADC full scale |
| **nFAULT** | Active-low, open-drain fault output of the driver |
| **DRVOFF** | Active-high input that turns all six driver FETs off |
| **nSLEEP** | Active-low enable/sleep input; a short low pulse also resets a latched fault |
| **PTC** | Positive-temperature-coefficient resettable fuse |
| **TVS** | Transient-voltage suppressor |
| **Rp / Rd** | USB-C pull-up (source) / pull-down (sink) on the CC pins |
| **DFU** | Device firmware upgrade; the STM32 ROM bootloader reachable over USB |
| **SWD** | Serial wire debug |
| **PGND** | The single power/analogue ground net on this board |
| **Netclass** | KiCad's per-net routing rules (width, clearance, via size) |

---

## 18. References

**Silicon and power**

* TI DRV8316 — three-phase integrated-FET motor driver: <https://www.ti.com/lit/ds/symlink/drv8316.pdf>
* TI TPS2116 — 2.5 A priority power mux: <https://www.ti.com/lit/ds/symlink/tps2116.pdf>
* TI TPS7A45 — 1.5 A low-dropout regulator: <https://www.ti.com/lit/ds/symlink/tps7a45.pdf>
* TI LMR36510 — 4.2–65 V, 1 A synchronous buck: <https://www.ti.com/lit/ds/symlink/lmr36510.pdf>
* ST STM32G431CB — Cortex-M4F MCU: <https://www.st.com/resource/en/datasheet/stm32g431cb.pdf>

**Discretes and passives**

* Nexperia BUK6Y33-60P — 60 V P-channel MOSFET
* Bourns MF-RHT series — high-temperature PTC fuses: <https://www.bourns.com/docs/product-datasheets/mf-rht.pdf>
* Littelfuse SP05xxBA — 4-channel ESD array: <https://www.littelfuse.com/~/media/files/littelfuse/technical%20resources/documents/data%20sheets/sp05xxba.pdf>
* Vishay SS14 — 1 A Schottky rectifier
* Bourns SRN6028 — shielded power inductor

**Sensing and connectors**

* ams AS5600 — 12-bit magnetic rotary position sensor
* GCT USB4110 — USB Type-C receptacle
* Phoenix Contact PT-1.5 series — 5.00 mm PCB terminal blocks

**Tools and standards**

* KiCad 10.0.5 (PCB format 20260206)
* IPC-2221 — generic standard on printed board design (track-width derating)

---

## 19. Revision history

| Version | Change |
|---|---|
| v6 | Baseline design: schematic, layout, DRC clean-up, fabrication outputs |
| v7 | **File/version rename only** — same schematic and layout as v6; folder and Gerber archive renamed to v7, project files still carry the `v6` name |

---

*Document generated from the v7 (v6-named) KiCad database, BOM and DRC reports. Where the hardware
and the documentation disagree, the hardware wins — and this document says so explicitly.*


