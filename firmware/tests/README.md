# PCB tests

Each folder is a complete, independent application. Build and flash one at a
time with `make APP=<name> flash` from the `firmware/` folder. Nothing here
starts motor current unless the test says so.

| Test | Covers | Safe with motor connected? |
|---|---|---|
| `test_00_hello` | 3V3, 170 MHz clock, USART1 TX (J7.1), nSLEEP/DRVOFF/nFAULT | yes |
| `test_01_as5600` | I2C1 PA15/PB7, AS5600 STATUS + RAW_ANGLE, hand-turn speed | yes |
| `test_02_drv8316` | DRV8316CT hardware mode, nSLEEP wake, nFAULT monitor & clear | yes (DRVOFF high) |
| `test_03_pwm` | TIM1 20 kHz, 3 complementary pairs, 120° modulation, dead time | yes (DRVOFF high) |
| `test_04_current_sense` | ADC1 PA0/1/2, TIM1_TRGO, DMA, zero-current offsets (0.3 V/A) | yes (MOE off) |
| `test_05_open_loop` | DRV enable sequence + V/f spin, optional encoder telemetry | **motor turns** |
| `test_06_encoder_cal` | Encoder sign/offset calibration + flash record | **rotor twitches** |
| `test_07_esp_link` | USART1 half-duplex (230400), frame CRC, ESP32 telemetry/command | yes (no motor needed) |
| `../foc` | Closed-loop sensored FOC (standalone) | **motor turns** |
| `../foc_esp` | Closed-loop sensored FOC + ESP32 WiFi telemetry & command | **motor turns (on enable)** |

## Expected output

### test_00_hello

```
bldc_control_final_v6  |  test_00_hello
SYSCLK     : 170000000 Hz
...
t=  1000 ms  nSLEEP=0  DRVOFF=1  nFAULT=1
```

If there is no output: check the 3V3 rail, the SWD connection, and that the
console is on **J7 pin 1 (PB6)** at 115200 8N1. J7 has no GND pin.

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
=== test_02_drv8316 (DRV8316CT Hardware Mode) ===
Hardware straps: MODE=6x PWM, GAIN=0.3 V/A, SLEW=125 V/us, OCP=16 A
DRVOFF is HIGH: motor outputs remain Hi-Z.

Waking DRV8316CT (nSLEEP low -> high)...
DRV8316_Init rc=0
DRV8316CT power-up OK, nFAULT de-asserted.

Monitoring nFAULT every 500 ms...

t=  1000 ms  nFAULT=0 (OK)  DRVOFF=1 (safe)
```

`rc=-1` means nFAULT is asserted. Check the VM supply voltage (4.5 to 35 V),
GND connection, and ensure no short circuit exists on the phase outputs.

### test_03_pwm

Scope INHA/INHB/INHC (or OUTA/B/C) and the complementary low sides:

* 20 kHz carrier (period 50 µs, ARR = 4249)
* three envelopes 120° apart, 0.5 Hz electrical so they are easy to see
* ~750 ns dead time between a high side and its low side

`PWM_TEST_ENABLE_OUTPUT` is 0 by default, so DRVOFF stays high and the power
stage is Hi-Z. Set it to 1 only with a current-limited supply.

### test_04_current_sense

```
Offsets: SOA=1.6500 V  SOB=1.6500 V  SOC=1.6500 V
  raw A/B/C        V A/B/C              I A/B/C (A)
```

With the bridge off, current should be ~0 A and each SOx ~1.65 V. The test
uses `i = (V - offset) / 0.3`; the FOC additionally flips the sign with
`FOC_CURRENT_SIGN = -1`.

### test_05_open_loop

```
Aligning for 1000 ms at mod=0.012...
Ramping frequency. Motor should now turn.
   t_ms    fe_Hz   mod    enc_deg    rpm   nFAULT
```

The rotor aligns, then ramps to 20 Hz electrical (~170 rpm mechanical) in
10 s. If it hums without turning, power down: the phase order or a phase
connection is wrong. There is no current loop here — the modulation is kept
small on purpose (this motor is 0.109 Ω).

### test_06_encoder_cal

```
Encoder_Calibrate rc=0
  sign   = +1
  offset = 1.2345 rad (70.73 deg elec)
  pole_pairs = 7, valid = 1
Saved to the last flash page (0x0801F800).
```

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

## After the tests

Run `make APP=foc flash` for standalone operation, or `make APP=foc_esp flash`
to control the motor over WiFi via the ESP32 bridge.
