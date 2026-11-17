# Control

Current-mode sensored FOC. The implementation lives in
[`firmware/foc/`](../firmware/foc/).

## Signal chain

Per 20 kHz cycle:

```
ADC (SOA, SOB, SOC)
  -> ia, ib, ic
  -> Clarke
  -> Park (angle from AS5600)
  -> PI on id, PI on iq
  -> inverse Park
  -> DPWMMIN (bottom-clamped) modulation
  -> TIM1 CH1/2/3 and CH1N/2N/3N
```

> **Status on the current board: experimental.** DRV8316 pin 35 (OCP/SR) is
> left floating, which enables ASR/AAR active demagnetisation. During bring-up
> the bridge passed almost no current whenever all three phases switched, so
> plain sine or SVPWM cannot drive the motor. The firmware uses DPWMMIN
> instead: the lowest phase is held at exactly 0 %, so its low side stays on
> with no edges. In the bring-up firmware this was only a partial workaround.
> On a spinning motor the current still dropped to zero in some 60° sectors.
> 6-step (`test_05_open_loop`) is the only drive verified end to end on this
> board. See [Modulation](#modulation).

The angle comes from `encoder.c`: the AS5600 RAW_ANGLE register is read at
4 kHz (every fifth control cycle), unwrapped, filtered for speed, and
extrapolated to the current control instant. The calibration record supplies
the direction sign and the electrical offset.

Phase mapping is fixed by the PCB:

```
TIM1_CH3 -> INHA -> OUTA -> SOA -> adc[0] -> ia
TIM1_CH2 -> INHB -> OUTB -> SOB -> adc[1] -> ib
TIM1_CH1 -> INHC -> OUTC -> SOC -> adc[2] -> ic
```

`Board_SetPwmRaw(ch1, ch2, ch3)`, `Board_SetPwmPhases(da, db, dc)` and
`write_duties(da, db, dc)` already use this order. If the wiring changes, change both the PWM order and the ADC rank
order.

## Peripheral configuration

TIM1, 20 kHz centre-aligned:

| Setting | Value |
|---|---|
| Prescaler | 0 |
| Counter mode | Centre-aligned 1 |
| ARR | 4249 |
| Repetition counter | 0 |
| Channels | CH1/2/3 plus CH1N/2N/3N |
| Dead time | `0x80 \| 21` = 170 ticks = 1.0 µs (SLEW 125 V/µs; the DRV adds none in 6x mode) |
| OSSR / OSSI | Enabled |
| Automatic output | Disabled |
| Break | Disabled; nFAULT is polled (2 ms debounce) |
| Gate pins | AF with pull-downs: an undriven pin reads INH = INL = 0 (Hi-Z) |

CH4 is not routed to a pin. It is configured as PWM mode 2 with
`CCR4 = ARR - 1`, and `CR2.MMS = OC4REF` routes its rising edge to
TIM1_TRGO at the counter peak. That is where all three low-side FETs are on
and the shunts can be sampled.

ADC1:

| Setting | Value |
|---|---|
| Clock | Synchronous, PCLK/4 = 42.5 MHz |
| Resolution | 12 bit |
| Scan | Enabled, three ranks |
| Trigger | TIM1_TRGO, rising |
| Rank 1/2/3 | IN1 / IN2 / IN3, 6.5 cycles |
| DMA | Circular, half-word, length 3 |
| Overrun | Overwrite |

The DMA transfer-complete callback runs once per PWM period and calls
`FOC_ControlISR()`.

## Parameters

Defined in `foc/foc.h`:

| Constant | Value | Meaning |
|---|---|---|
| `FOC_VBUS` | 24.0 V | Assumed bus voltage (no VM sense; bring-up ran at 9 V: `USER_DEFS=-DFOC_VBUS=9.0f`) |
| `FOC_USE_SVPWM` | 0 | 0 = DPWMMIN, 1 = min-max SVPWM (only once pin 35 is grounded) |
| `FOC_VMAX_FRAC` | 0.45 | Phase voltage limit, fraction of Vbus |
| `FOC_I_MAX` | 4.0 A | Peak dq current limit |
| `FOC_KP` | 0.43 V/A | Current-loop proportional gain |
| `FOC_KI` | 690 V/(A s) | Current-loop integral gain |
| `FOC_ALIGN_MOD_MAX` | 0.02 | Open-loop alignment clamp |
| `FOC_CSA_V_PER_A` | 0.3 V/A | DRV8316 CSA gain (GAIN pin open), from `DRV8316_CSA_V_PER_A` in `board.h` |
| `FOC_PWM_ARR` | 4249 | 20 kHz at 170 MHz |

`FOC_CURRENT_SIGN` in `foc.c` sets the shunt polarity. It is **+1**, as
measured on the board: with `i = (V_SOx - offset) / 0.3`, driving phase A at
10 % with B and C at 0 % (stalled motor, 9 V) read A = +2.9 A and
B = C = -1.6 A. SOx rises for current flowing out of the phase into the motor.

`FOC_CalibrateOffsets()` rejects offsets outside 1.3 to 2.0 V (measured
1.64 V). SOx reads 0 V while the DRV sleeps or has no VM, so a bad offset
usually means the driver is not awake.

## Current loop

The plant is approximately `1 / (L s + R)` per axis:

```
fc = 500 to 1000 Hz
KP = L  * 2 pi * fc    [V/A]
KI = R  * 2 pi * fc    [V/(A s)]
```

For this motor (Rph = 0.109 ohm, Ls = 68 uH) at fc = 1 kHz:

```
KP = 68e-6 * 2 pi * 1000 = 0.43 V/A
KI = 0.109  * 2 pi * 1000 = 690 V/(A s)
```

If the loop is sluggish, raise KP. If it overshoots or buzzes, lower KP and
then KI. The control loop runs at 20 kHz, but the PWM update is once per half
period, so 1 to 1.5 kHz is a sensible bandwidth ceiling.

Both integrators are clamped to the voltage limit and use back-calculation,
so a saturated vector does not wind up.

## Modulation

Bring-up measurements with fixed duties on all three phases (stalled motor,
9 V, gate read-back correct, nFAULT high in every case):

| A / B / C duty | A phase held at 0 %? | Current |
|---|---|---|
| 10 / 0 / 0 | yes | flows: A +2.9 A, B and C −1.6 A |
| 20 / 0 / 10 | yes | flows: A +3.9 A, B −5 A (ADC saturated) |
| 20 / 2 / 10 | no, B at 2 % | partial, about 0.4 A mean |
| 60 / 40 / 50, 30 / 10 / 20 | no | about 0 A |

Rule: current flows only when at least one phase's low side is held on with
no PWM edges. 60/40/50 commands the same A–B voltage as 20/0/10 and gives
0 A, so dead time does not explain it. The working hypothesis is ASR/AAR,
enabled because OCP/SR (pin 35) is open (DRV8316 datasheet §8.3.12). The fix
is to tie pin 35 to GND (level 1: ASR/AAR off, OCP 16 A) and power-cycle.
**Do not** tie it to AVDD: that level keeps AAR on.

Until that rework is done:

- `foc.c` uses **DPWMMIN** (`Board_DutyDpwmMin()`): `d_k = (v_k − v_min) / Vbus`.
  The lowest phase is at exactly 0 %. There is no minimum duty, because 2 %
  already blocked most of the current.
- The encoder calibration vector and `test_03_pwm` use the same modulation.
- `test_05_open_loop` uses 6-step. Only one phase switches in 6-step, and it
  is the drive that worked on the board. See [`trapezoidal.md`](trapezoidal.md).

Measured on 2026-11-18 with `test_08_drive_lab`: sine and SVPWM pass
0.01 A, 6-step and DPWM run to 180 Hz, I/f current control on SOx works to
about 43 Hz, and sensorless FOC locks but loses conduction above about 40 Hz.
The board also shows **no dead-time voltage loss** (ASR), and phases float near
their current zero crossings. Details and numbers:
[`results/2026-11-18_drive_lab.md`](results/2026-11-18_drive_lab.md).

After pin 35 is grounded, re-run the raw-duty table above (60/40/50 should
give about the same current as 20/0/10). Then build FOC with
`USER_DEFS=-DFOC_USE_SVPWM=1`.

## Sampling window

The DRV8316 has low-side current sense, so SOx is valid only while that
phase's low-side FET is on. The ADC is triggered at the counter peak, where
every channel in PWM mode 1 is inactive and every low side is on. The usable
time after the peak is:

```
t_usable = (1 - d) * T/2 - t_dead
```

With DPWMMIN at `FOC_VMAX_FRAC = 0.45` the highest duty is the line-to-line
voltage, up to `sqrt(3) * 0.45 = 0.78`. With T = 50 us and 1 us dead time:

```
t_usable = (1 - 0.78) * 25 us - 1 us = 4.5 us
```

Three ADC ranks at 6.5 cycles take about 1.35 us. The clamped phase is valid
for the whole period. The phase with the highest duty has the shortest window,
so `FOC_ControlISR()` rebuilds it from the other two (`ia + ib + ic = 0`),
the same way the bring-up sine ISR did.

The 1 us dead time matches the **125 V/us** slew strap (47 kΩ to AVDD).
The board layer clears driver faults before the bridge is enabled.

If more voltage or speed is needed later:

- Lower the switching frequency to 15 kHz. The window grows to about 3.9 us,
  at the cost of about 2.9 A peak-to-peak ripple.
- Use two-shunt sampling and compute the third current. The ADC time halves
  to about 0.9 us.
- Move to 40 kHz only with two-shunt sampling or external current sense.

## Encoder angle

| Item | Value |
|---|---|
| I2C read at 400 kHz | about 40 us |
| Sample rate (every 5th cycle) | 4 kHz |
| Worst-case sample age | about 290 us |
| Angle error at 1000 rpm without extrapolation | about 12 electrical degrees |
| Angle error at 2000 rpm without extrapolation | about 24 electrical degrees |
| AS5600 practical speed limit | about 1000 rpm |

`RAW_ANGLE` is used instead of the filtered `ANGLE` to avoid the 0.29 to
2.2 ms slow-filter lag.

## Optional speed loop

The final app commands torque only. If a speed loop is added, run it at about
1 kHz, not at 20 kHz. `w_ref` is mechanical rad/s: 1000 rpm = 104.7,
2000 rpm = 209.4. With the AS5600 keep the reference at or below about
104 rad/s.

```c
static float s_spd_i;

void SpeedLoop(float w_ref, float iq_max)
{
    float sgn = Encoder_GetCal()->sign;
    float e   = w_ref - Encoder_MechanicalSpeed();
    float iq;

    s_spd_i += 20.0f * 0.001f * e;
    if (s_spd_i >  iq_max) { s_spd_i =  iq_max; }
    if (s_spd_i < -iq_max) { s_spd_i = -iq_max; }

    iq = 0.02f * e + s_spd_i;
    if (iq >  iq_max) { iq =  iq_max; }
    if (iq < -iq_max) { iq = -iq_max; }

    FOC_SetCurrentRef(0.0f, sgn * iq);
}
```

The `sgn` factor matters: if the encoder is mounted opposite to the phase
order, `sign` is -1 and positive `iq` produces negative mechanical torque.

## Protections in the final app

- nFAULT is polled every 5 ms. A persistent fault disables the bridge.
- The encoder I2C error rate is checked every 200 ms.
- `iq` ramps at 0.5 A/s to a fixed reference.
- Above 800 rpm the reference is dropped to zero and the motor coasts.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| id and iq always zero | ADC not triggered, MOE off, DRVOFF high, or all three phases switching (pin 35 floating, see Modulation) |
| Currents inverted | `FOC_CURRENT_SIGN` |
| Idle current offset above 0.3 A | offsets not calibrated with the bridge off |
| Rotor vibrates, does not turn | encoder calibration failed or wrong pole pairs |
| Runs away or turns the wrong way | encoder sign or offset is wrong |
| Overcurrent immediately on enable | phase mapping, offsets, or too much voltage |
| `DRV8316_Init` fails | VM missing (no VIN on J1), DRVOFF held high, nSLEEP |
| CSA offsets rejected | DRV asleep or unpowered: SOx reads 0 V |
| nFAULT low | DRVOFF high, about 1.8 ms after a wake, UVLO, OCP, overtemperature |
| Current drops out in some sectors | ASR/AAR with pin 35 floating, see Modulation |
| Angle jitters | magnet gap, magnet status, I2C errors |

## Known limits

- **AS5600 speed range.** Rated speed is 2000 rpm, the encoder is good to
  about 1000 rpm. The app guards at 800 rpm.
- **Current range.** At 0.3 V/A the ADC saturates at about ±5.4 A, so the
  limit is 4.0 A peak, below the motor's 7.07 A rated peak.
- **Modulation.** Pin 35 is floating, so the drive is DPWMMIN and only
  partly works. See Modulation.
- **No Vbus sense.** `FOC_VBUS` is a constant, so the modulation limit tracks
  the real supply only if it stays at 24 V.
- **No speed or position loop.** `FOC_SetCurrentRef()` is the only input.
- **No field weakening.** `id` is normally zero.
- **No hardware break.** nFAULT is polled in software.
