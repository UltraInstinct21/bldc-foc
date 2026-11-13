# Motor

The firmware constants are sized for the 24 V outrunner described below. The
source is the motor design report, which is intentionally not part of this
repository; see `.gitignore`.

## Specification

| Item | Value |
|---|---|
| DC bus | 24 V |
| Topology | Outer rotor, 14 poles (p = 7), 12 slots, 3 phases |
| Rated current | 5 Arms (7.07 A peak), 5.4 Arms / 7.6 A peak hot |
| Rated torque | 0.50 Nm (0.47 Nm hot) |
| Rated speed | 2000 rpm (233 Hz electrical) |
| No-load speed | about 2780 rpm cold, 3000 rpm hot |
| Speed at rated current | about 2600 rpm |
| Phase resistance | 0.093 ohm at 20 C, 0.109 ohm at 65 C, 0.118 ohm hot |
| Phase inductance | 68 uH (Ld about Lq, surface PM) |
| PM flux linkage | 6.79 mVs cold, 6.30 mVs hot |
| Back-EMF constant | Ke = 0.0475 Vs/rad phase peak |
| Torque constant | Kt = 0.0713 Nm/Apeak cold, 0.0662 Nm/Apeak hot |
| Kv | 116 rpm/V |
| Pole pairs | 7 |
| Winding | 34 turns/phase, 2 parallel paths, AWG 22, kw = 0.966 |
| Magnet | 30 x 8 x 3 mm N35 |
| Efficiency | about 89 percent at 2000 rpm |

## Derived limits

| Item | Value |
|---|---|
| Phase voltage at 2000 rpm | 10.7 V peak, 77 percent of Vdc/sqrt(3) |
| Stall current | 127 A peak |
| Three-phase short-circuit current | about 100 A peak |
| Cogging | 84 periods/rev, 5 to 10 mNm |
| Current ripple at 20 kHz | 2.2 A peak-to-peak, 31 percent of peak |
| Current ripple at 40 kHz | 1.1 A peak-to-peak |

## Constants derived from the motor

| Constant | Value | Reason |
|---|---|---|
| `MOTOR_POLE_PAIRS` | 7 | 14 poles |
| `FOC_VBUS` | 24.0 V | Design bus |
| `FOC_VMAX_FRAC` | 0.45 | 10.8 V phase peak, enough for 2000 rpm |
| `FOC_KP` | 0.43 V/A | Ls * 2 pi * 1 kHz |
| `FOC_KI` | 690 V/(A s) | Rph * 2 pi * 1 kHz |
| `FOC_I_MAX` | 7.0 A | Rated peak current; CSA is linear to +/-8 A |
| `FOC_CSA_V_PER_A` | 0.15 V/A | +/-8 A range |
| `FOC_PWM_ARR` | 4249 | 20 kHz centre-aligned at 170 MHz |
| `FOC_TS` | 50 us | 20 kHz control period |

`FOC_I_MAX` is a peak dq limit. 7 A peak is 5 A RMS, the continuous rating.
Do not command 7 A as a DC current or run there without checking the winding
temperature.

## Consequences

1. **The AS5600 cannot reliably follow 2000 rpm.** Its practical limit is
   about 1000 rpm. Sustained rated speed needs an ABZ encoder (PA15 and
   PB12, TIM8 encoder mode) or an SPI absolute encoder.
2. **The current-sense amplifier is nearly at its range.** 0.15 V/A gives
   +/-8 A and the motor draws 7.07 A peak at rated torque. `FOC_I_MAX`
   protects both.
3. **20 kHz gives 2.2 A peak-to-peak ripple.** The design report recommends
   40 kHz or accepting the ripple. 20 kHz is the right bring-up choice
   because the low-side sampling window at high duty is already small.
4. **Voltage headroom is thin at 2000 rpm.** 10.7 V of the 13.86 V linear
   limit leaves little room for IR drop and control margin. There is no
   Vbus sense, so a sagging supply reduces achievable speed.
5. **The DRV8316 must run at 200 V/us slew.** At the 25 V/us reset default
   the internal dead time leaves no usable sampling window at high duty.
