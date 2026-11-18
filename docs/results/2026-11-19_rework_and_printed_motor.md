# Pin 35/36 rework and first runs of the 3D-printed motor, 2026-11-18/19

Continuation of [`2026-11-18_drive_lab.md`](2026-11-18_drive_lab.md). Covers:

1. the DRV8316 rework (pins 35 and 36 to GND) and the faults it caused;
2. the drive modes re-tested with ASR/AAR off;
3. the first runs of the user's 3D-printed 12N14P motor.

Raw console logs:
[`2026-11-18_pin35_grounded.log`](2026-11-18_pin35_grounded.log) (sections 2),
[`2026-11-18_printed_motor.log`](2026-11-18_printed_motor.log) (section 3).
Firmware: `test_09_drv_diag`, `test_05_open_loop`, `test_08_drive_lab`.

---

## 1. Rework: DRV8316 pins 35 (OCP/SR) and 36 (GAIN) to GND

Intent: pin 35 to GND turns ASR/AAR off (all-phase switching should conduct);
pin 36 to GND sets the CSA gain to 0.15 V/A (range ≈ ±10.9 A).

`test_09_drv_diag` (analog nFAULT, SOx and 3V3 readings) through the repair:

| When | nFAULT after wake | SOA / SOB / SOC awake (V) | Finding |
|---|---|---|---|
| Before rework | low 1678 µs, then high | 1.650 / 1.652 / 1.650 | healthy |
| Right after rework | **stuck low**, ignores nSLEEP pulse and DRVOFF | 0.19 / 0.19 / 0.31 | persistent supply-side fault; nFAULT net itself OK (3.29 V asleep) |
| After first repair | stuck low | **1.655** / 0.27 / 0.31 | VREF (pin 37) OK again; SOB, SOC loaded |
| After second repair | **low 1654 µs, clears, follows DRVOFF** | 1.655 / 1.655 / **0.33 (asleep 0.34)** | driver OK; SOC (pin 38) open |
| Latest | low 1620 µs, OK | 1.643 / **0.234 (asleep 0.194)** / 1.646 | SOC fixed; **SOB (pin 39) now open** |

Signature of an open SOx pin: the same ~0.2–0.5 V asleep and awake (healthy:
~0 V asleep, VREF/2 ≈ 1.65 V awake). The firmware now detects an offset outside
1.3–2.0 V and rebuilds that phase from the other two (ia + ib + ic = 0).

**CSA gain 0.15 V/A confirmed**: at standstill, the duty needed for "2 A" was
the same as before the rework at 0.3 V/A (a wrong constant would have halved it).
`DRV8316_CSA_V_PER_A = 0.15f` in `board.h`.

---

## 2. Drive modes with pin 35 grounded (drone motor, 9 V)

### Static duty table (`raw`, stalled motor, one CSA rebuilt)

| A / B / C duty | Before (pin 35 open) | After (pin 35 to GND) |
|---|---|---|
| 10 / 0 / 0 | A +3.21, B −1.63 A | A +3.42, B −1.73 A |
| 26 / 14 / 20 | 0 A | **A +2.64, B −2.50 A** |
| 56 / 44 / 50 | 0 A | **A +2.59, B −2.44 A** |
| 55 / 45 / 50 | 0 A | **A +1.95, B −1.85 A** |
| 12 / 2 / 6 | +0.17 A (partial) | **+1.95 A** |

All-phase switching now conducts: the ASR/AAR block is gone.

### Open loop, 40 % amplitude, 2 A limit, ramp toward 200 Hz el.

| Mode | Before | After |
|---|---|---|
| Sine | 0.01 A, interlock trip | **runs to ~180 Hz el.**, current flowing 85–99 % of samples |
| SVPWM | 0.01 A, interlock trip | **runs to ~180 Hz el.**, current flowing 85–99 % |

### Phase C high side (found during these runs)

| A / B / C duty | iA | iB | rebuilt iC | Meaning |
|---|---|---|---|---|
| 10 / 0 / 0 | +3.36 | −1.71 | −1.65 | A high, B and C low OK |
| 0 / 10 / 0 | −1.71 | +3.43 | −1.72 | B high OK |
| **0 / 0 / 10** | −0.01 | 0.00 | 0 | **C high side did not conduct** |
| 10 / 10 / 0 | +1.65 | +1.68 | −3.33 | C low side OK |

Rebuilt C RMS was 0.5–0.8 A against 1.6–1.7 A on A/B. Suspected INHC (pin 31)
open. In the later printed-motor runs (section 3.3) all three phases carried
equal RMS current in 6-step, so this appears fixed; re-run `raw 0 0 10` to
confirm.

---

## 3. 3D-printed motor

| Item | Value |
|---|---|
| Construction | 3D-printed stator and rotor (no iron), 14 poles / 12 slots → 7 pole pairs |
| Rated (design) | 24 V, 5 A, 50,000 rpm |
| Simulation | 1 A at 2,480 rpm (≈ 290 Hz el.) |
| Line-to-line resistance | **0.154 Ω** (0.1 V / 0.65 A) |
| Supply used | 12 V and 24 V bench supply |

50,000 rpm = 5,833 Hz el. = one 6-step step every 28.6 µs, shorter than the
50 µs PWM period: out of reach for this board/firmware. The first target was
the simulated 2,480 rpm point.

### 3.1 With series resistors in each motor lead

| R per lead | Supply | Duty | Expected current | Result |
|---|---|---|---|---|
| 68 Ω | 12 V | 40 % | ~35 mA avg (88 mA max) | no movement (PSU 0.064 A incl. board) |
| 68 Ω | 24 V | 95 % | ~170 mA | no movement |
| 18 Ω | 24 V | 50 % | ~0.33 A (≈4 W per resistor) | no movement |
| 18 Ω | 12 V | 95 % | ~0.32 A | **rotor resists by hand during the hold, no rotation** |
| 18 Ω | 12 V | 95 % | ~0.32 A, ramp 1 → 290 Hz | no rotation |

Measured line-to-line with the 18 Ω resistors fitted: 35.6 Ω (= 2 × 18 + 0.154 ✓).

**Firmware cannot see this current.** With tens of ohms in series, L/R is a few
microseconds: current flows only during each PWM on-pulse and is ~0 at the ADC
sampling instant (counter peak). Readings stayed at 0.01–0.02 A whatever the
real current was, so the firmware limiter and trip are blind; the resistors are
the only protection. The PSU display is the real current meter in this setup.

Conclusion: ≤ 0.33 A gives holding torque but not enough to break the rotor's
static friction.

### 3.2 First run note

The first 68 Ω run started with the lab's default settings (target 200 Hz,
ramp 5 + 0.1·f) instead of the planned 10 Hz: the board had reset and the
settings, which live in RAM, were lost. Harmless with 68 Ω (≤ 0.18 A possible),
but since then every run loads and **checks** its settings in the same session
before `start` (`tools/lab.py` `check:` steps).

### 3.3 Without series resistors, 12 V (the motor spins)

6-step, ramp from 1 Hz, `accel 5 0.1`, target 290 Hz el., 1 s alignment.
Phase current is continuous without the resistors, so the CSA and the
firmware limiter work.

**Run A/B: duty ceiling 15 %, current limit 3 A** (run twice, same result):

| f el. | ≈ rpm | Duty | I (filtered) | Phase RMS A / B / C |
|---|---|---|---|---|
| 1 Hz | 9 | 7.9 % | 3.0 A | — |
| 103 Hz | 883 | 10.6 % | 3.0 A | 2.47 / 2.46 / 2.42 A |
| 203 Hz | 1,740 | 14.3 % | 3.0 A | 2.47 / 2.47 / 2.41 A |
| 229 Hz | 1,963 | **14.8 %** (at the 15 % ceiling) | 3.0 A | |
| 259 Hz | 2,216 | 7.1 %, 5.8 A spike | — | sync lost |
| 290 Hz | 2,486 | 8.4 % | 3.0 A | no back-EMF: stalled / vibrating |

- The rotor **followed to ~220–229 Hz el. (≈ 1,900–1,960 rpm)**. The duty
  needed for 3 A rose from 7.9 % to 14.8 % (back-EMF), then hit the 15 %
  ceiling; above it current and torque fell and the rotor slipped.
- PSU read ≈ 0.5 A max (≈ 3 A × 15 % + board).

**Run C: duty ceiling 30 %, current limit 4.5 A** (the lab's maximum `ilim`):

| f el. | ≈ rpm | Duty | I filtered / peak sample |
|---|---|---|---|
| 1 Hz | 9 | 11.7 % | 4.48 / 4.63 A |
| 103 Hz | 883 | 14.7 % | 4.48 / 5.18 A |
| 203 Hz | 1,740 | 18.2 % | 4.42 / 5.61 A |
| 229 Hz | 1,963 | 16.8 % (dip: slip starting) | 4.39 / 5.92 A |
| 259 Hz | 2,216 | 21.0 % | 4.73 / 6.23 A |
| ~265 Hz | ~2,270 | — | **overcurrent trip** (> 5.3 A for 20 ms), max sample 7.78 A |

- More current moved the slip point from ~229 to ~260 Hz el. but did not
  remove it; the firmware trip stopped the run cleanly.
- 7.78 A is close to the DRV8316's 8 A peak rating: more current is not the
  way forward.

### 3.4 Derived motor data (open-loop estimates)

| Quantity | Value | How |
|---|---|---|
| Loop resistance (motor + driver + wiring) | ≈ 0.32 Ω | 7.9 % × 12 V / 3 A at standstill; consistent at 4.5 A (11.7 %) |
| Share of that in the motor | 0.154 Ω | measured; the rest is DRV8316 R_DS(on) (~0.19 Ω per conducting pair) and leads |
| Back-EMF at 203 Hz el. | ≈ 0.8 V line-to-line (average over a step) | duty rise at constant current: (14.3 − 7.9) % × 12 V; same at 4.5 A |
| Flux linkage λ | ≈ 0.0004 Wb | from the back-EMF above; about half the drone motor's 0.0009 Wb, as expected without iron |
| Inductance | high enough for continuous current at 20 kHz without resistors | CSA readings tracked the limiter; not measured (`rl` not run) |

### 3.5 Next steps for this motor

1. **Gentler ramp near the top** (the limit now is open-loop acceleration, not
   current): `amp 30 ilim 4.5 f 290 accel 5 0.02` (≈ 10 Hz/s at 290 Hz instead
   of 30 Hz/s; 1 → 290 Hz takes ~35 s). Check stator and board temperature
   first: run C put ≈ 4 W into the windings and ≈ 2 W into the driver for 20 s.
2. Hold just below the slip point (`f 200`) for 10 s to confirm stable running.
3. Measure R and L with `rl` (no resistors) and λ with `if`, then try sine /
   SVPWM open loop and I/f, now that pin 35 is grounded.
4. Repair SOB (pin 39) and confirm phase C with `raw 0 0 10`.
5. Beyond ~3,000 rpm: closed-loop commutation (observer or sensor) and a
   higher PWM frequency; 50,000 rpm needs a different design.

### 3.6 Safety notes from these runs

- Series resistors cap current but make it invisible to the firmware; the PSU
  limit then caps **power**, not phase current (phase ≈ supply ÷ duty).
- Without resistors at 12 V, 1 % duty is ≈ 0.8 A at standstill: keep the duty
  ceiling low (15–30 %) and let the firmware limiter set the current.
- The lab clamps `ilim` to 4.5 A; the overcurrent trip is fixed at 5.3 A.
- PLA softens at 55–60 °C: check the printed stator after every run.

## Reproduce

```sh
cd firmware
make APP=test_08_drive_lab flash
python3 tools/lab.py --log ../docs/results/run.log \
    "cmd:vbus 12" "cmd:mode 6step" "cmd:amp 30" "cmd:ilim 4.5" "cmd:fstart 1" \
    "cmd:accel 5 0.1" "cmd:align 1000" "cmd:f 290" "cmd:log 1000" \
    "check:amp 30.0%" "check:ilim 4.50" "check:f 290.0 Hz" "check:mode 6step" spin:22
```
