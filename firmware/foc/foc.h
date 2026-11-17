/**
 * @file    foc.h
 * @brief   Minimal sensored FOC for bldc_control_final_v6 (STM32G431 + DRV8316T).
 *
 * Current-mode FOC: ADC -> Clarke -> Park -> 2x PI -> inverse Park
 *                   -> DPWMMIN (bottom-clamped) modulation -> TIM1.
 * Rotor angle comes from encoder.c (AS5600).
 *
 * Motor (from bldc_design_calculations.pdf):
 *   24 V bus, 14 poles (7 pole pairs), 5 Arms / 7.07 A peak rated,
 *   Rph = 0.109 ohm, Ls = 68 uH, lambda_pm = 6.79 mVs, 2000 rpm, 0.50 Nm.
 *
 * The DRV8316T is strapped in hardware mode:
 *   6x PWM, CSA gain 0.3 V/A, VREF/2 = 1.65 V, SLEW = 125 V/us (47k strap),
 *   OCP/SR (pin 35) floating -> ASR/AAR active demagnetisation enabled.
 * There is no SPI link; the firmware only drives nSLEEP and DRVOFF and
 * reads nFAULT. The ADC saturates at about +/-5.4 A at 0.3 V/A, so
 * FOC_I_MAX is set well below that.
 *
 * Modulation: with pin 35 floating, the bridge passes almost no current when
 * all three phases switch (bring-up log, raw-duty tests). So the default is
 * DPWMMIN: the lowest phase is held at exactly 0 % (low side on, no edges).
 * In the bring-up firmware this was only a partial workaround: on a
 * spinning motor the current still dropped out in some 60-degree sectors.
 * Treat FOC on this board as experimental until pin 35 is tied to GND;
 * then build with USER_DEFS=-DFOC_USE_SVPWM=1.
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
#include "board.h"

/* ---- board / motor constants -------------------------------------------- */
#ifndef FOC_VBUS
#define FOC_VBUS            24.0f       /* motor design bus voltage; the board
                                         * has no VM sense. Bring-up ran at 9 V:
                                         * USER_DEFS=-DFOC_VBUS=9.0f */
#endif

/* 0 = DPWMMIN (default, needed while DRV8316 pin 35 is floating).
 * 1 = min-max SVPWM, only after pin 35 has been tied to GND. */
#ifndef FOC_USE_SVPWM
#define FOC_USE_SVPWM       0
#endif

#define FOC_PWM_ARR         4249u       /* 170 MHz / (2*(4249+1)) = 20 kHz */
#define FOC_TS              (1.0f / 20000.0f)

#define FOC_CSA_V_PER_A     DRV8316_CSA_V_PER_A     /* board.h, 0.3 V/A */
#define FOC_ADC_LSB         (3.3f / 4095.0f)
#define FOC_I_PER_COUNT     (FOC_ADC_LSB / FOC_CSA_V_PER_A)   /* ~2.69 mA */

/* Maximum phase-voltage amplitude as a fraction of Vbus.
 *
 * At 2000 rpm the motor needs 10.7 V phase-peak (77 % of Vdc/sqrt(3)),
 * i.e. 10.7/24 = 0.45 of Vbus. With DPWMMIN the highest duty is the
 * line-to-line voltage, up to sqrt(3) * 0.45 = 0.78, leaving
 *
 *     (1 - 0.78) * 25 us - 1 us dead time = 4.5 us
 *
 * of low-side window after the counter peak. Three ADC ranks take
 * ~1.35 us. The clamped phase is valid all period; the highest-duty phase
 * is rebuilt from the other two (ia + ib + ic = 0) in any case.
 */
#ifndef FOC_VMAX_FRAC
#define FOC_VMAX_FRAC       0.45f
#endif
#define FOC_VMAX            (FOC_VMAX_FRAC * FOC_VBUS)

/* Peak current limit, A (dq vector magnitude).
 *
 * DRV8316T GAIN pin is open (0.3 V/A), bias VREF/2 = 1.65 V (1.64 V measured).
 * The CSA is linear to about +/-4.6 A and the ADC saturates at ~+/-5.4 A;
 * the DRV8316 itself is rated 8 A peak. Clamp to 4.0 A. */
#ifndef FOC_I_MAX
#define FOC_I_MAX           4.0f
#endif
#define FOC_I_MAX_HARD_CEILING 4.5f

/* Maximum modulation for the open-loop alignment vector.
 *
 * At standstill the current is set by R, not by the inductance. With DPWMMIN
 * the theta = 0 vector switches only phase A at duty 1.5 * mod, and the 1 us
 * dead time removes about 2 % of it, so the effective duty is roughly
 * 1.5 * mod - 0.02 (zero below mod = 0.013). For the 0.109 ohm motor at
 * 24 V, 0.03 gives ~4.3 A through A -> B||C. This is a typo-safety ceiling;
 * raise it with USER_DEFS for a higher-resistance or lower-voltage setup. */
#ifndef FOC_ALIGN_MOD_MAX
#define FOC_ALIGN_MOD_MAX   0.03f
#endif

/* No minimum duty: the clamped phase must sit at exactly 0 %. A small
 * non-zero duty (dead time eats INH, INL still toggles) was measured to
 * block most of the current with pin 35 floating. */
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
 * Bridge must be disabled (MOE off) and the ADC/DMA already running.
 * Returns 0 and marks FOC ready only if every offset is inside
 * DRV8316_CSA_OFF_MIN_V..MAX_V (1.3..2.0 V, expect ~1.64 V); -1 otherwise
 * (DRV asleep or unpowered, SOx not connected). */
int   FOC_CalibrateOffsets(const volatile uint16_t *adc, uint32_t n);

/* References, amps. id is normally 0. */
void  FOC_SetCurrentRef(float id, float iq);

/* Enable / disable the bridge. Disable drops MOE (all INx low = Hi-Z,
 * motor coasts). DRVOFF is left low on purpose; see board.h.
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
 * modulation is clamped to FOC_ALIGN_MOD_MAX (0.03) so a standstill vector
 * stays near 4 A on the design motor; see the note there on dead time. Bypasses the control loop; do not call while
 * FOC_ControlISR() is running. */
void  FOC_ApplyOpenLoopVector(float theta_e, float modulation);

/* Telemetry. */
float FOC_GetId(void);
float FOC_GetIq(void);

const FocPi *FOC_GetPiD(void);
const FocPi *FOC_GetPiQ(void);

#endif /* FOC_H */
