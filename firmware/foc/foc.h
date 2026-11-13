/**
 * @file    foc.h
 * @brief   Minimal sensored FOC for bldc_control_final_v6 (STM32G431 + DRV8316R).
 *
 * Current-mode FOC: ADC -> Clarke -> Park -> 2x PI -> inverse Park -> SVPWM -> TIM1.
 * Rotor angle comes from encoder.c (AS5600).
 *
 * Motor (from bldc_design_calculations.pdf):
 *   24 V bus, 14 poles (7 pole pairs), 5 Arms / 7.07 A peak rated,
 *   Rph = 0.109 ohm, Ls = 68 uH, lambda_pm = 6.79 mVs, 2000 rpm, 0.50 Nm.
 *
 * The DRV8316CT is strapped in hardware mode:
 *   6x PWM, CSA gain 0.3 V/A, VREF/2 = 1.65 V, SLEW = 125 V/us (47k strap).
 * There is no SPI link; the firmware only drives nSLEEP and DRVOFF and
 * reads nFAULT. The 0.3 V/A gain limits the linear current range to about
 * +/-4.6 A, so FOC_I_MAX is set below that.
 *
 * PWM: TIM1 centre-aligned, 20 kHz, CH1/2/3 + CHxN.
 *   CH1 -> INHC -> OUTC
 *   CH2 -> INHB -> OUTB
 *   CH3 -> INHA -> OUTA
 * CH4 (no pin) is used only to trigger the ADC at the centre of the
 * low-side conduction window.
 */
#ifndef FOC_H
#define FOC_H

#include <stdint.h>
#include "stm32g4xx_hal.h"

/* ---- board / motor constants -------------------------------------------- */
#ifndef FOC_VBUS
#define FOC_VBUS            24.0f       /* motor design bus voltage */
#endif

#define FOC_PWM_ARR         4249u       /* 170 MHz / (2*(4249+1)) = 20 kHz */
#define FOC_TS              (1.0f / 20000.0f)

#define FOC_CSA_V_PER_A     0.3f        /* GAIN pin Hi-Z on DRV8316CT */
#define FOC_ADC_LSB         (3.3f / 4095.0f)
#define FOC_I_PER_COUNT     (FOC_ADC_LSB / FOC_CSA_V_PER_A)   /* ~2.69 mA */

/* Maximum phase-voltage amplitude as a fraction of Vbus.
 *
 * At 2000 rpm the motor needs 10.7 V phase-peak (77 % of Vdc/sqrt(3)),
 * i.e. 10.7/24 = 0.45 of Vbus. With min-max SVPWM the peak duty is
 * 0.5 + 0.75*frac = 0.84, leaving
 *
 *     (1 - 0.84) * 25 us - 1.5 us dead time = 2.5 us
 *
 * of low-side window after the counter peak. Three ADC ranks at
 * 6.5 + 12.5 cycles take ~1.35 us, so there is ~1.2 us of margin.
 *
 * This only works because drv8316.c sets SLEW = 200 V/us (dead time
 * 0.5-0.75 us instead of 1.8-3.4 us at the 25 V/us default).
 */
#ifndef FOC_VMAX_FRAC
#define FOC_VMAX_FRAC       0.45f
#endif
#define FOC_VMAX            (FOC_VMAX_FRAC * FOC_VBUS)

/* Peak current limit, A (dq vector magnitude).
 *
 * DRV8316CT GAIN pin is strapped Hi-Z (0.3 V/A), giving VREF/2 = 1.65 V mid-rail.
 * Linear CSA output is 0.15 V to 3.15 V (swing = +/-1.5 V).
 * Maximum linear phase current = 1.5 V / 0.3 V/A = +/-4.6 A.
 * To prevent ADC saturation, clipping, and integrator windup, clamp to 4.0 A. */
#ifndef FOC_I_MAX
#define FOC_I_MAX           4.0f
#endif
#define FOC_I_MAX_HARD_CEILING 4.5f

/* Maximum modulation for the open-loop alignment vector.
 *
 * At standstill the current is set by Rph, not by the inductance:
 *   I = mod * Vbus / Rph.
 * This motor has Rph = 0.109 ohm, so 1 % of 24 V is already ~2.2 A.
 * 2 % (~4.4 A) is a hard ceiling for a typo-safe alignment. */
#ifndef FOC_ALIGN_MOD_MAX
#define FOC_ALIGN_MOD_MAX   0.02f
#endif

#define FOC_DUTY_MIN        0.02f
#define FOC_DUTY_MAX        0.98f

/* Current-loop gains (V/A and V/(A*s)). Tune on the bench; see
 * docs/control.md, current loop.
 *
 * Motor: Rph = 0.109 ohm, Ls = 68 uH, target fc = 1 kHz:
 *   KP = Ls * 2*pi*fc = 0.43 V/A
 *   KI = Rph * 2*pi*fc = 690 V/(A*s)                                    */
#ifndef FOC_KP
#define FOC_KP              0.43f
#endif
#ifndef FOC_KI
#define FOC_KI              690.0f
#endif

typedef struct
{
    float kp;
    float ki;
    float integ;
} FocPi;

/* ---- API ---------------------------------------------------------------- */

/* Bind the timer and reset state. Call after MX_TIM1_Init(). */
void  FOC_Init(TIM_HandleTypeDef *htim1);

/* Average n ADC samples and store the zero-current offsets.
 * Bridge must be disabled (MOE off) and the ADC/DMA already running. */
void  FOC_CalibrateOffsets(const volatile uint16_t *adc, uint32_t n);

/* References, amps. id is normally 0. */
void  FOC_SetCurrentRef(float id, float iq);

/* Enable / disable the bridge. Disable drops MOE (motor coasts).
 * FOC_Enable() does nothing until FOC_CalibrateOffsets() has run. */
void  FOC_Enable(void);
void  FOC_Disable(void);

uint8_t FOC_IsReady(void);
uint8_t FOC_IsEnabled(void);

/* Run one control cycle. Call from the ADC DMA-complete callback (20 kHz).
 * Also advances the encoder (calls Encoder_Tick()). */
void  FOC_ControlISR(const volatile uint16_t *adc);

/* Apply a stationary voltage vector, used only for encoder calibration.
 * theta_e in rad, modulation = peak phase voltage / Vbus. mod <= 0 disables.
 * modulation is clamped to FOC_ALIGN_MOD_MAX (0.02) so a standstill vector
 * cannot exceed ~4.4 A. Bypasses the control loop; do not call while
 * FOC_ControlISR() is running. */
void  FOC_ApplyOpenLoopVector(float theta_e, float modulation);

/* Telemetry. */
float FOC_GetId(void);
float FOC_GetIq(void);

const FocPi *FOC_GetPiD(void);
const FocPi *FOC_GetPiQ(void);

#endif /* FOC_H */
