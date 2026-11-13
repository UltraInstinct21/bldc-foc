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
  -> min-max SVPWM
  -> TIM1 CH1/2/3 and CH1N/2N/3N
```

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

`Board_SetPwmRaw(ch1, ch2, ch3)` and `write_duties(da, db, dc)` already use
this order. If the wiring changes, change both the PWM order and the ADC rank
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
| Dead time | 0x80, about 753 ns |
| OSSR / OSSI | Enabled |
| Automatic output | Disabled |
| Break | Disabled; nFAULT is polled |

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
| `FOC_VBUS` | 24.0 V | Assumed bus voltage |
| `FOC_VMAX_FRAC` | 0.45 | Phase voltage limit, fraction of Vbus |
| `FOC_I_MAX` | 7.0 A | Peak dq current limit |
| `FOC_KP` | 0.43 V/A | Current-loop proportional gain |
| `FOC_KI` | 690 V/(A s) | Current-loop integral gain |
| `FOC_ALIGN_MOD_MAX` | 0.02 | Open-loop alignment clamp |
| `FOC_CSA_V_PER_A` | 0.15 V/A | DRV8316 CSA gain |
| `FOC_PWM_ARR` | 4249 | 20 kHz at 170 MHz |

`FOC_CURRENT_SIGN` in `foc.c` flips the shunt polarity. The DRV8316 marks SOx
positive for current into the low-side FET, so inverter-to-motor current is
negative during the sampling window; the default is -1.

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

## Sampling window

The DRV8316 has low-side shunts and internal dead time. In centre-aligned PWM
the usable time after the counter peak is:

```
t_usable = (1 - d) * T/2 - t_dead
```

At `FOC_VMAX_FRAC = 0.45` the peak duty is
`0.5 + 0.75 * 0.45 = 0.8375`. With T = 50 us:

```
t_usable = (1 - 0.8375) * 25 us - 1.5 us = 2.56 us
```

Three ADC ranks at 6.5 cycles take about 1.35 us, leaving about 1.2 us of
margin. The low-side FETs have also been on for about 2.5 us before the
trigger, which covers the CSA settling time.

This only works with the **125 V/us** slew rate (hardware strapped with 47 kΩ to AVDD).
The board layer initializes and clears driver faults before the bridge is enabled.

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
| id and iq always zero | ADC not triggered, MOE off, DRVOFF high |
| Currents inverted | `FOC_CURRENT_SIGN` |
| Idle current offset above 0.3 A | offsets not calibrated with the bridge off |
| Rotor vibrates, does not turn | encoder calibration failed or wrong pole pairs |
| Runs away or turns the wrong way | encoder sign or offset is wrong |
| Overcurrent immediately on enable | phase mapping, offsets, or too much voltage |
| `DRV8316_Init` fails | SPI mode, 16-bit size, PA4 GPIO, nSLEEP, 3V3 |
| SPI parity or clock fault | frame format, CPHA, data size |
| nFAULT low | UVLO, OCP, overtemperature |
| Angle jitters | magnet gap, magnet status, I2C errors |

## Known limits

- **AS5600 speed range.** Rated speed is 2000 rpm, the encoder is good to
  about 1000 rpm. The app guards at 800 rpm.
- **Current range.** The CSA is linear to +/-8 A at 0.15 V/A, so the limit is
  7.0 A peak. There is little headroom above rated torque.
- **No Vbus sense.** `FOC_VBUS` is a constant, so the modulation limit tracks
  the real supply only if it stays at 24 V.
- **No speed or position loop.** `FOC_SetCurrentRef()` is the only input.
- **No field weakening.** `id` is normally zero.
- **No hardware break.** nFAULT is polled in software.
