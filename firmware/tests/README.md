# PCB tests

Each folder is a complete, independent application. Build and flash one at a
time with `make APP=<name> flash` from the `firmware/` folder: USB-C DFU when
the board is on USB, otherwise ST-Link. Output appears on the USB serial port
(`/dev/serial/by-id/usb-Custom_STM32G431_*-if00`), or on J7 pin 1 when no
terminal has that port open. Nothing here starts motor current unless the
test says so. `Ctrl-C` or `ESC` on the USB console turns the bridge off.

| Test | Covers | Safe with motor connected? |
|---|---|---|
| `test_00_hello` | 3V3, 170 MHz clock, USB console, USART1 TX (J7.1), nSLEEP/DRVOFF/nFAULT | yes |
| `test_01_as5600` | I2C1 PA15/PB7, AS5600 STATUS + RAW_ANGLE, hand-turn speed | yes |
| `test_02_drv8316` | DRV8316T wake, 30 µs fault-clear pulse, DRVOFF→nFAULT check, monitor | yes (INx low) |
| `test_03_pwm` | TIM1 20 kHz, 3 complementary pairs, DPWMMIN 120° modulation, 1 µs dead time | yes (DRV asleep) |
| `test_04_current_sense` | DRV awake, ADC1 PA0/1/2, TIM1_TRGO, DMA, zero-current offsets (0.3 V/A) | yes (MOE off) |
| `test_05_open_loop` | Open-loop 6-step with current limiter, trip and auto-retry (verified drive) | **motor turns** |
| `test_06_encoder_cal` | Encoder sign/offset calibration + flash record | **rotor twitches** |
| `test_07_esp_link` | USART1 half-duplex (230400), frame CRC, ESP32 telemetry/command | yes (no motor needed) |
| `test_08_drive_lab` | Interactive: 6-step / sine / SVPWM / DPWM, raw duty table, R/L from SOx, I/f current control, sensorless FOC, fault trace | **motor turns** (on command) |
| `../foc` | Closed-loop sensored FOC (standalone), experimental while pin 35 floats | **motor turns** |
| `../foc_esp` | Closed-loop sensored FOC + ESP32 WiFi telemetry & command (same caveat) | **motor turns (on enable)** |

## Expected output

### test_00_hello

```
bldc_control_final_v6  |  test_00_hello
SYSCLK     : 170000000 Hz
...
t=  1000 ms  nSLEEP=0  DRVOFF=0  nFAULT=1
```

DRVOFF is **0** at idle. The bridge is off because all six INx are low;
DRVOFF high would pull nFAULT low.

If there is no output: check the 3V3 rail and the USB cable (C-to-A works
best), or read the fallback console on **J7 pin 1 (PB6)** at 115200 8N1.
J7 has no GND pin.

### test_01_as5600

```
AS5600 found. STATUS=0x20  MD=1 ML=0 MH=0
  raw   angle_deg   rpm
    0       0.00       0.0
```

`STATUS=0x20` means the magnet is detected. `0x00` means no magnet.
Turn the shaft by hand: `raw` must sweep 0..4095 exactly once per turn.

No answer at 0x36: 4.7 k pull-ups missing, SDA on the wrong pin, or the
module is not powered. PB7 is shared with USART1_RX; this firmware never
enables standard UART RX on PB7.

### test_02_drv8316

```
=== test_02_drv8316 (DRV8316T Hardware Mode) ===
Straps: MODE=6x PWM, SLEW=125 V/us, GAIN=0.3 V/A, OCP/SR open (ASR/AAR on)
Idle: DRVOFF LOW, all INx low -> outputs Hi-Z.

Waking DRV8316T (nSLEEP low -> high)...
  nFAULT low for 2 ms after wake (expect ~2 ms)
DRV8316_Init rc=0
DRV8316T awake, nFAULT high.
DRVOFF high: nFAULT=0 (expect 0)   DRVOFF low: nFAULT=1 (expect 1)
DRVOFF check: PASS

Monitoring nFAULT every 500 ms (1 = OK)...

t=  1000 ms  nFAULT=1 (OK)  DRVOFF=0  INx=0 (Hi-Z)
```

`rc=-1` with nFAULT stuck low usually means VM is missing: there is no VIN on
J1 (check TP2/TP3, F1, Q1). Without VM the chip clamps nFAULT low and ignores
nSLEEP, so the DRVOFF check also fails.

### test_03_pwm

Scope INHA/INHB/INHC (or OUTA/B/C) and the complementary low sides:

* 20 kHz carrier (period 50 µs, ARR = 4249)
* three envelopes 120° apart, 0.5 Hz electrical so they are easy to see
* bottom-clamped: in every 120° sector one phase sits at 0 % (INH low, INL high)
* 1.0 µs dead time between a high side and its low side

`PWM_TEST_ENABLE_OUTPUT` is 0 by default, so the DRV stays asleep and the
power stage is Hi-Z. Set it to 1 only with a current-limited supply.

### test_04_current_sense

```
Offsets: SOA=1.6400 V  SOB=1.6400 V  SOC=1.6400 V
  raw A/B/C        V A/B/C              I A/B/C (A)
```

The test wakes the DRV8316, because SOx reads 0 V while the driver sleeps.
With the bridge off, current should be about 0 A and each SOx about 1.64 V
(valid window 1.3–2.0 V). The test and the FOC both use
`i = (V - offset) / 0.3`, positive for current flowing out of the phase into
the motor (`FOC_CURRENT_SIGN = +1`, measured).

### test_05_open_loop

```
=== test_05_open_loop (6-step, current limited) ===
duty 40 %  ilim 2.0 A  f 4 -> 200 Hz el.  align 500 ms
CSA offsets A 1.640 B 1.640 C 1.640 V -> OK
   t_s  state  rpm    f_Hz  steps/s  duty%  I_A  (peak)  nFAULT
   1.0  RUN       41     4.8       25    6.0  0.80 (1.20)  high
```

This is the 6-step drive that was verified on the board: a drone motor at
9 V ran 4 → 200 Hz electrical in about 12 s with a 2 A limit. Measured
points (duty, phase current avg/peak): 15 Hz 6 % 0.8/1.2 A, 60 Hz 9.7 %
0.9/2.1 A, 100 Hz 14.5 % 1.7/2.7 A, 200 Hz 24 % 1.7/2.9 A. The duty rising
with speed shows back-EMF tracking. Low speed (15–60 Hz) is rough, and high
speed is smooth.

`I_A` is phase current, measured on the phase whose low side is held on.
The supply sees about duty × phase current, so 0.3–0.5 A on the PSU is
normal. Overcurrent (above min(2 × ilim, 5.3 A) for 20 ms) stops the bridge
and retries after 3 s. nFAULT stops it and retries after 1 s. Override the
profile with `USER_DEFS`, for example `-DT5_FMAX_HZ=100.0f -DT5_ILIM_A=1.5f`.
`rpm` assumes 7 pole pairs (`T5_POLE_PAIRS`).

Full documentation (step table, timer mechanics, current sensing, limiter,
start-up, measurements, tuning, troubleshooting):
[`docs/trapezoidal.md`](../../docs/trapezoidal.md).

Why 6-step: DRV8316 pin 35 is floating, so the bridge passes almost no
current when all three phases switch. See
[`docs/control.md`](../../docs/control.md#modulation).

### test_06_encoder_cal

```
Encoder_Calibrate rc=0
  sign   = +1
  offset = 1.2345 rad (70.73 deg elec)
  pole_pairs = 7, valid = 1
Saved to the last flash page (0x0801F800).
```

The alignment vector is bottom-clamped (lowest phase at 0 %), because with
pin 35 floating a vector made with all three phases switching passes almost
no current. Its angle convention (`theta = 0` → phase A axis) matches
`FOC_ApplyOpenLoopVector()`. An earlier version of this test used a
90°-shifted convention, so recalibrate if the record was written by it.

`-3` means the rotor did not follow the alignment vector: check the phase
wiring, the magnet, and that the shaft is free. Do not raise `CAL_MOD`
above 0.01–0.02; at standstill that is already ~2.2–4.4 A.

### test_07_esp_link

Verifies the half-duplex UART link on PB6 (J7.1) at 230400 baud with the external ESP32.
No motor needed.

Expected:
- The ESP32 web dashboard shows **ONLINE** badge.
- Encoder angle field counts upwards 0..4095.
- Adjusting the `iq ref` slider on the web UI echoes back immediately in the dashboard.

### test_08_drive_lab

Interactive over the USB console (`help` lists the commands), or scripted
with `tools/lab.py`:

```sh
make APP=test_08_drive_lab flash
python3 tools/lab.py --log ../docs/results/run.log raw 6step sine svpwm dpwm rl if
```

Results of the first run on the board, with explanations, are in
[`docs/results/2026-11-18_drive_lab.md`](../../docs/results/2026-11-18_drive_lab.md).
In short, with DRV8316 pin 35 floating: 6-step and DPWM run, sine and SVPWM
pass no current, I/f current control on SOx works to ~43 Hz, and sensorless
FOC locks and runs briefly but loses conduction above ~40 Hz.

## After the tests

Run `make APP=foc flash` for standalone operation, or `make APP=foc_esp flash`
to control the motor over WiFi via the ESP32 bridge.
